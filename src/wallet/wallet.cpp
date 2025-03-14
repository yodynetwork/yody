// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>

#include <chain.h>
#include <consensus/consensus.h>
#include <consensus/validation.h>
#include <external_signer.h>
#include <fs.h>
#include <interfaces/chain.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <key_io.h>
#include <outputtype.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <psbt.h>
#include <script/descriptor.h>
#include <script/script.h>
#include <script/signingprovider.h>
#include <txmempool.h>
#include <util/bip32.h>
#include <util/check.h>
#include <util/error.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <util/rbf.h>
#include <util/string.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/external_signer_scriptpubkeyman.h>
#include <miner.h>
#include <node/blockstorage.h>
#include <yody/yodyledger.h>

#include <univalue.h>

#include <algorithm>
#include <assert.h>
#include <optional>

#include <boost/algorithm/string/replace.hpp>

using interfaces::FoundBlock;

const std::map<uint64_t,std::string> WALLET_FLAG_CAVEATS{
    {WALLET_FLAG_AVOID_REUSE,
        "You need to rescan the blockchain in order to correctly mark used "
        "destinations in the past. Until this is done, some destinations may "
        "be considered unused, even if the opposite is the case."
    },
};

RecursiveMutex cs_wallets;
static std::vector<std::shared_ptr<CWallet>> vpwallets GUARDED_BY(cs_wallets);
static std::list<LoadWalletFn> g_load_wallet_fns GUARDED_BY(cs_wallets);

bool AddWalletSetting(interfaces::Chain& chain, const std::string& wallet_name)
{
    util::SettingsValue setting_value = chain.getRwSetting("wallet");
    if (!setting_value.isArray()) setting_value.setArray();
    for (const util::SettingsValue& value : setting_value.getValues()) {
        if (value.isStr() && value.get_str() == wallet_name) return true;
    }
    setting_value.push_back(wallet_name);
    return chain.updateRwSetting("wallet", setting_value);
}

bool RemoveWalletSetting(interfaces::Chain& chain, const std::string& wallet_name)
{
    util::SettingsValue setting_value = chain.getRwSetting("wallet");
    if (!setting_value.isArray()) return true;
    util::SettingsValue new_value(util::SettingsValue::VARR);
    for (const util::SettingsValue& value : setting_value.getValues()) {
        if (!value.isStr() || value.get_str() != wallet_name) new_value.push_back(value);
    }
    if (new_value.size() == setting_value.size()) return true;
    return chain.updateRwSetting("wallet", new_value);
}

static void UpdateWalletSetting(interfaces::Chain& chain,
                                const std::string& wallet_name,
                                std::optional<bool> load_on_startup,
                                std::vector<bilingual_str>& warnings)
{
    if (!load_on_startup) return;
    if (load_on_startup.value() && !AddWalletSetting(chain, wallet_name)) {
        warnings.emplace_back(Untranslated("Wallet load on startup setting could not be updated, so wallet may not be loaded next node startup."));
    } else if (!load_on_startup.value() && !RemoveWalletSetting(chain, wallet_name)) {
        warnings.emplace_back(Untranslated("Wallet load on startup setting could not be updated, so wallet may still be loaded next node startup."));
    }
}

bool AddWallet(const std::shared_ptr<CWallet>& wallet)
{
    LOCK(cs_wallets);
    assert(wallet);
    std::vector<std::shared_ptr<CWallet>>::const_iterator i = std::find(vpwallets.begin(), vpwallets.end(), wallet);
    if (i != vpwallets.end()) return false;
    vpwallets.push_back(wallet);
    wallet->ConnectScriptPubKeyManNotifiers();
    wallet->NotifyCanGetAddressesChanged();
    return true;
}

bool RemoveWallet(const std::shared_ptr<CWallet>& wallet, std::optional<bool> load_on_start, std::vector<bilingual_str>& warnings)
{
    assert(wallet);

    interfaces::Chain& chain = wallet->chain();
    std::string name = wallet->GetName();

    // Unregister with the validation interface which also drops shared ponters.
    wallet->m_chain_notifications_handler.reset();
    LOCK(cs_wallets);
    std::vector<std::shared_ptr<CWallet>>::iterator i = std::find(vpwallets.begin(), vpwallets.end(), wallet);
    if (i == vpwallets.end()) return false;
    vpwallets.erase(i);

    // Write the wallet setting
    UpdateWalletSetting(chain, name, load_on_start, warnings);

    return true;
}

bool RemoveWallet(const std::shared_ptr<CWallet>& wallet, std::optional<bool> load_on_start)
{
    std::vector<bilingual_str> warnings;
    return RemoveWallet(wallet, load_on_start, warnings);
}

std::vector<std::shared_ptr<CWallet>> GetWallets()
{
    LOCK(cs_wallets);
    return vpwallets;
}

std::shared_ptr<CWallet> GetWallet(const std::string& name)
{
    LOCK(cs_wallets);
    for (const std::shared_ptr<CWallet>& wallet : vpwallets) {
        if (wallet->GetName() == name) return wallet;
    }
    return nullptr;
}

std::unique_ptr<interfaces::Handler> HandleLoadWallet(LoadWalletFn load_wallet)
{
    LOCK(cs_wallets);
    auto it = g_load_wallet_fns.emplace(g_load_wallet_fns.end(), std::move(load_wallet));
    return interfaces::MakeHandler([it] { LOCK(cs_wallets); g_load_wallet_fns.erase(it); });
}

static Mutex g_loading_wallet_mutex;
static Mutex g_wallet_release_mutex;
static std::condition_variable g_wallet_release_cv;
static std::set<std::string> g_loading_wallet_set GUARDED_BY(g_loading_wallet_mutex);
static std::set<std::string> g_unloading_wallet_set GUARDED_BY(g_wallet_release_mutex);

// Custom deleter for shared_ptr<CWallet>.
static void ReleaseWallet(CWallet* wallet)
{
    const std::string name = wallet->GetName();
    wallet->WalletLogPrintf("Releasing wallet\n");
    wallet->StopStake();
    wallet->Flush();
    delete wallet;
    // Wallet is now released, notify UnloadWallet, if any.
    {
        LOCK(g_wallet_release_mutex);
        if (g_unloading_wallet_set.erase(name) == 0) {
            // UnloadWallet was not called for this wallet, all done.
            return;
        }
    }
    g_wallet_release_cv.notify_all();
}

void UnloadWallet(std::shared_ptr<CWallet>&& wallet)
{
    // Mark wallet for unloading.
    const std::string name = wallet->GetName();
    {
        LOCK(g_wallet_release_mutex);
        auto it = g_unloading_wallet_set.insert(name);
        assert(it.second);
    }
    // The wallet can be in use so it's not possible to explicitly unload here.
    // Notify the unload intent so that all remaining shared pointers are
    // released.
    wallet->NotifyUnload();

    // Time to ditch our shared_ptr and wait for ReleaseWallet call.
    wallet.reset();
    {
        WAIT_LOCK(g_wallet_release_mutex, lock);
        while (g_unloading_wallet_set.count(name) == 1) {
            g_wallet_release_cv.wait(lock);
        }
    }
}

namespace {
std::shared_ptr<CWallet> LoadWalletInternal(interfaces::Chain& chain, const std::string& name, std::optional<bool> load_on_start, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    try {
        std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
        if (!database) {
            error = Untranslated("Wallet file verification failed.") + Untranslated(" ") + error;
            return nullptr;
        }

        chain.initMessage(_("Loading wallet…").translated);
        std::shared_ptr<CWallet> wallet = CWallet::Create(&chain, name, std::move(database), options.create_flags, error, warnings);
        if (!wallet) {
            error = Untranslated("Wallet loading failed.") + Untranslated(" ") + error;
            status = DatabaseStatus::FAILED_LOAD;
            return nullptr;
        }
        AddWallet(wallet);
        wallet->postInitProcess();

        // Write the wallet setting
        UpdateWalletSetting(chain, name, load_on_start, warnings);

        return wallet;
    } catch (const std::runtime_error& e) {
        error = Untranslated(e.what());
        status = DatabaseStatus::FAILED_LOAD;
        return nullptr;
    }
}
} // namespace

std::shared_ptr<CWallet> LoadWallet(interfaces::Chain& chain, const std::string& name, std::optional<bool> load_on_start, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    auto result = WITH_LOCK(g_loading_wallet_mutex, return g_loading_wallet_set.insert(name));
    if (!result.second) {
        error = Untranslated("Wallet already loading.");
        status = DatabaseStatus::FAILED_LOAD;
        return nullptr;
    }
    auto wallet = LoadWalletInternal(chain, name, load_on_start, options, status, error, warnings);
    WITH_LOCK(g_loading_wallet_mutex, g_loading_wallet_set.erase(result.first));
    return wallet;
}

std::shared_ptr<CWallet> CreateWallet(interfaces::Chain& chain, const std::string& name, std::optional<bool> load_on_start, DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    uint64_t wallet_creation_flags = options.create_flags;
    const SecureString& passphrase = options.create_passphrase;

    if (wallet_creation_flags & WALLET_FLAG_DESCRIPTORS) options.require_format = DatabaseFormat::SQLITE;

    // Indicate that the wallet is actually supposed to be blank and not just blank to make it encrypted
    bool create_blank = (wallet_creation_flags & WALLET_FLAG_BLANK_WALLET);

    // Born encrypted wallets need to be created blank first.
    if (!passphrase.empty()) {
        wallet_creation_flags |= WALLET_FLAG_BLANK_WALLET;
    }

    // Private keys must be disabled for an external signer wallet
    if ((wallet_creation_flags & WALLET_FLAG_EXTERNAL_SIGNER) && !(wallet_creation_flags & WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        error = Untranslated("Private keys must be disabled when using an external signer");
        status = DatabaseStatus::FAILED_CREATE;
        return nullptr;
    }

    // Descriptor support must be enabled for an external signer wallet
    if ((wallet_creation_flags & WALLET_FLAG_EXTERNAL_SIGNER) && !(wallet_creation_flags & WALLET_FLAG_DESCRIPTORS)) {
        error = Untranslated("Descriptor support must be enabled when using an external signer");
        status = DatabaseStatus::FAILED_CREATE;
        return nullptr;
    }

    // Wallet::Verify will check if we're trying to create a wallet with a duplicate name.
    std::unique_ptr<WalletDatabase> database = MakeWalletDatabase(name, options, status, error);
    if (!database) {
        error = Untranslated("Wallet file verification failed.") + Untranslated(" ") + error;
        status = DatabaseStatus::FAILED_VERIFY;
        return nullptr;
    }

    // Do not allow a passphrase when private keys are disabled
    if (!passphrase.empty() && (wallet_creation_flags & WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        error = Untranslated("Passphrase provided but private keys are disabled. A passphrase is only used to encrypt private keys, so cannot be used for wallets with private keys disabled.");
        status = DatabaseStatus::FAILED_CREATE;
        return nullptr;
    }

    // Make the wallet
    chain.initMessage(_("Loading wallet…").translated);
    std::shared_ptr<CWallet> wallet = CWallet::Create(&chain, name, std::move(database), wallet_creation_flags, error, warnings);
    if (!wallet) {
        error = Untranslated("Wallet creation failed.") + Untranslated(" ") + error;
        status = DatabaseStatus::FAILED_CREATE;
        return nullptr;
    }

    // Encrypt the wallet
    if (!passphrase.empty() && !(wallet_creation_flags & WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        if (!wallet->EncryptWallet(passphrase)) {
            error = Untranslated("Error: Wallet created but failed to encrypt.");
            status = DatabaseStatus::FAILED_ENCRYPT;
            return nullptr;
        }
        if (!create_blank) {
            // Unlock the wallet
            if (!wallet->Unlock(passphrase)) {
                error = Untranslated("Error: Wallet was encrypted but could not be unlocked");
                status = DatabaseStatus::FAILED_ENCRYPT;
                return nullptr;
            }

            // Set a seed for the wallet
            {
                LOCK(wallet->cs_wallet);
                if (wallet->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
                    wallet->SetupDescriptorScriptPubKeyMans();
                } else {
                    for (auto spk_man : wallet->GetActiveScriptPubKeyMans()) {
                        if (!spk_man->SetupGeneration()) {
                            error = Untranslated("Unable to generate initial keys");
                            status = DatabaseStatus::FAILED_CREATE;
                            return nullptr;
                        }
                    }
                }
            }

            // Relock the wallet
            wallet->Lock();
        }
    }
    AddWallet(wallet);
    wallet->postInitProcess();

    // Write the wallet settings
    UpdateWalletSetting(chain, name, load_on_start, warnings);

    status = DatabaseStatus::SUCCESS;
    return wallet;
}

/** @defgroup mapWallet
 *
 * @{
 */

const CWalletTx* CWallet::GetWalletTx(const uint256& hash) const
{
    AssertLockHeld(cs_wallet);
    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(hash);
    if (it == mapWallet.end())
        return nullptr;
    return &(it->second);
}

void CWallet::UpgradeKeyMetadata()
{
    if (IsLocked() || IsWalletFlagSet(WALLET_FLAG_KEY_ORIGIN_METADATA)) {
        return;
    }

    auto spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        return;
    }

    spk_man->UpgradeKeyMetadata();
    SetWalletFlag(WALLET_FLAG_KEY_ORIGIN_METADATA);
}

void CWallet::UpgradeDescriptorCache()
{
    if (!IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS) || IsLocked() || IsWalletFlagSet(WALLET_FLAG_LAST_HARDENED_XPUB_CACHED)) {
        return;
    }

    for (ScriptPubKeyMan* spkm : GetAllScriptPubKeyMans()) {
        DescriptorScriptPubKeyMan* desc_spkm = dynamic_cast<DescriptorScriptPubKeyMan*>(spkm);
        desc_spkm->UpgradeDescriptorCache();
    }
    SetWalletFlag(WALLET_FLAG_LAST_HARDENED_XPUB_CACHED);
}

bool CWallet::Unlock(const SecureString& strWalletPassphrase, bool accept_no_keys)
{
    CCrypter crypter;
    CKeyingMaterial _vMasterKey;

    {
        LOCK(cs_wallet);
        for (const MasterKeyMap::value_type& pMasterKey : mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, _vMasterKey))
                continue; // try another master key
            if (Unlock(_vMasterKey, accept_no_keys)) {
                // Now that we've unlocked, upgrade the key metadata
                UpgradeKeyMetadata();
                // Now that we've unlocked, upgrade the descriptor cache
                UpgradeDescriptorCache();
                return true;
            }
        }
    }
    return false;
}

bool CWallet::ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase)
{
    bool fWasLocked = IsLocked();

    {
        LOCK(cs_wallet);
        Lock();

        CCrypter crypter;
        CKeyingMaterial _vMasterKey;
        for (MasterKeyMap::value_type& pMasterKey : mapMasterKeys)
        {
            if(!crypter.SetKeyFromPassphrase(strOldWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                return false;
            if (!crypter.Decrypt(pMasterKey.second.vchCryptedKey, _vMasterKey))
                return false;
            if (Unlock(_vMasterKey))
            {
                int64_t nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = static_cast<unsigned int>(pMasterKey.second.nDeriveIterations * (100 / ((double)(GetTimeMillis() - nStartTime))));

                nStartTime = GetTimeMillis();
                crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod);
                pMasterKey.second.nDeriveIterations = (pMasterKey.second.nDeriveIterations + static_cast<unsigned int>(pMasterKey.second.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime)))) / 2;

                if (pMasterKey.second.nDeriveIterations < 25000)
                    pMasterKey.second.nDeriveIterations = 25000;

                WalletLogPrintf("Wallet passphrase changed to an nDeriveIterations of %i\n", pMasterKey.second.nDeriveIterations);

                if (!crypter.SetKeyFromPassphrase(strNewWalletPassphrase, pMasterKey.second.vchSalt, pMasterKey.second.nDeriveIterations, pMasterKey.second.nDerivationMethod))
                    return false;
                if (!crypter.Encrypt(_vMasterKey, pMasterKey.second.vchCryptedKey))
                    return false;
                WalletBatch(GetDatabase()).WriteMasterKey(pMasterKey.first, pMasterKey.second);
                if (fWasLocked)
                    Lock();
                return true;
            }
        }
    }

    return false;
}

void CWallet::chainStateFlushed(const CBlockLocator& loc)
{
    WalletBatch batch(GetDatabase());
    batch.WriteBestBlock(loc);
}

void CWallet::SetMinVersion(enum WalletFeature nVersion, WalletBatch* batch_in)
{
    LOCK(cs_wallet);
    if (nWalletVersion >= nVersion)
        return;
    nWalletVersion = nVersion;

    {
        WalletBatch* batch = batch_in ? batch_in : new WalletBatch(GetDatabase());
        if (nWalletVersion > 40000)
            batch->WriteMinVersion(nWalletVersion);
        if (!batch_in)
            delete batch;
    }
}

std::set<uint256> CWallet::GetConflicts(const uint256& txid) const
{
    std::set<uint256> result;
    AssertLockHeld(cs_wallet);

    std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(txid);
    if (it == mapWallet.end())
        return result;
    const CWalletTx& wtx = it->second;

    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;

    for (const CTxIn& txin : wtx.tx->vin)
    {
        if (mapTxSpends.count(txin.prevout) <= 1)
            continue;  // No conflict if zero or one spends
        range = mapTxSpends.equal_range(txin.prevout);
        for (TxSpends::const_iterator _it = range.first; _it != range.second; ++_it)
            result.insert(_it->second);
    }
    return result;
}

bool CWallet::HasWalletSpend(const uint256& txid) const
{
    AssertLockHeld(cs_wallet);
    auto iter = mapTxSpends.lower_bound(COutPoint(txid, 0));
    return (iter != mapTxSpends.end() && iter->first.hash == txid);
}

void CWallet::Flush()
{
    GetDatabase().Flush();
}

void CWallet::Close()
{
    StopStake();
    GetDatabase().Close();
}

void CWallet::SyncMetaData(std::pair<TxSpends::iterator, TxSpends::iterator> range)
{
    // We want all the wallet transactions in range to have the same metadata as
    // the oldest (smallest nOrderPos).
    // So: find smallest nOrderPos:

    int nMinOrderPos = std::numeric_limits<int>::max();
    const CWalletTx* copyFrom = nullptr;
    for (TxSpends::iterator it = range.first; it != range.second; ++it) {
        const CWalletTx* wtx = &mapWallet.at(it->second);
        if (wtx->nOrderPos < nMinOrderPos) {
            nMinOrderPos = wtx->nOrderPos;
            copyFrom = wtx;
        }
    }

    if (!copyFrom) {
        return;
    }

    // Now copy data from copyFrom to rest:
    for (TxSpends::iterator it = range.first; it != range.second; ++it)
    {
        const uint256& hash = it->second;
        CWalletTx* copyTo = &mapWallet.at(hash);
        if (copyFrom == copyTo) continue;
        assert(copyFrom && "Oldest wallet transaction in range assumed to have been found.");
        if (!copyFrom->IsEquivalentTo(*copyTo)) continue;
        copyTo->mapValue = copyFrom->mapValue;
        copyTo->vOrderForm = copyFrom->vOrderForm;
        // fTimeReceivedIsTxTime not copied on purpose
        // nTimeReceived not copied on purpose
        copyTo->nTimeSmart = copyFrom->nTimeSmart;
        copyTo->fFromMe = copyFrom->fFromMe;
        // nOrderPos not copied on purpose
        // cached members not copied on purpose
    }
}

/**
 * Outpoint is spent if any non-conflicted transaction
 * spends it:
 */
bool CWallet::IsSpent(const uint256& hash, unsigned int n) const
{
    const COutPoint outpoint(hash, n);
    std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range;
    range = mapTxSpends.equal_range(outpoint);

    for (TxSpends::const_iterator it = range.first; it != range.second; ++it)
    {
        const uint256& wtxid = it->second;
        std::map<uint256, CWalletTx>::const_iterator mit = mapWallet.find(wtxid);
        if (mit != mapWallet.end()) {
            int depth = mit->second.GetDepthInMainChain();
            if (depth > 0  || (depth == 0 && !mit->second.isAbandoned()))
                return true; // Spent
        }
    }
    return false;
}

void CWallet::AddToSpends(const COutPoint& outpoint, const uint256& wtxid)
{
    mapTxSpends.insert(std::make_pair(outpoint, wtxid));

    setLockedCoins.erase(outpoint);

    std::pair<TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    SyncMetaData(range);
}

void CWallet::RemoveFromSpends(const COutPoint& outpoint, const uint256& wtxid)
{
    std::pair<TxSpends::iterator, TxSpends::iterator> range;
    range = mapTxSpends.equal_range(outpoint);
    TxSpends::iterator it = range.first;
    for(; it != range.second; ++ it)
    {
        if(it->second == wtxid)
        {
            mapTxSpends.erase(it);
            break;
        }
    }
    range = mapTxSpends.equal_range(outpoint);
    if(range.first != range.second)
        SyncMetaData(range);
}

void CWallet::AddToSpends(const uint256& wtxid)
{
    auto it = mapWallet.find(wtxid);
    assert(it != mapWallet.end());
    const CWalletTx& thisTx = it->second;
    if (thisTx.IsCoinBase()) // Coinbases don't spend anything!
        return;

    for (const CTxIn& txin : thisTx.tx->vin)
        AddToSpends(txin.prevout, wtxid);
}

void CWallet::RemoveFromSpends(const uint256& wtxid)
{
    assert(mapWallet.count(wtxid));
    CWalletTx& thisTx = mapWallet.at(wtxid);
	if (thisTx.IsCoinBase()) // Coinbases don't spend anything!
        return;

    for(const CTxIn& txin : thisTx.tx->vin)
        RemoveFromSpends(txin.prevout, wtxid);
}

bool CWallet::EncryptWallet(const SecureString& strWalletPassphrase)
{
    if (IsCrypted())
        return false;

    CKeyingMaterial _vMasterKey;

    _vMasterKey.resize(WALLET_CRYPTO_KEY_SIZE);
    GetStrongRandBytes(_vMasterKey.data(), WALLET_CRYPTO_KEY_SIZE);

    CMasterKey kMasterKey;

    kMasterKey.vchSalt.resize(WALLET_CRYPTO_SALT_SIZE);
    GetStrongRandBytes(kMasterKey.vchSalt.data(), WALLET_CRYPTO_SALT_SIZE);

    CCrypter crypter;
    int64_t nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, 25000, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = static_cast<unsigned int>(2500000 / ((double)(GetTimeMillis() - nStartTime)));

    nStartTime = GetTimeMillis();
    crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod);
    kMasterKey.nDeriveIterations = (kMasterKey.nDeriveIterations + static_cast<unsigned int>(kMasterKey.nDeriveIterations * 100 / ((double)(GetTimeMillis() - nStartTime)))) / 2;

    if (kMasterKey.nDeriveIterations < 25000)
        kMasterKey.nDeriveIterations = 25000;

    WalletLogPrintf("Encrypting Wallet with an nDeriveIterations of %i\n", kMasterKey.nDeriveIterations);

    if (!crypter.SetKeyFromPassphrase(strWalletPassphrase, kMasterKey.vchSalt, kMasterKey.nDeriveIterations, kMasterKey.nDerivationMethod))
        return false;
    if (!crypter.Encrypt(_vMasterKey, kMasterKey.vchCryptedKey))
        return false;

    {
        LOCK(cs_wallet);
        mapMasterKeys[++nMasterKeyMaxID] = kMasterKey;
        WalletBatch* encrypted_batch = new WalletBatch(GetDatabase());
        if (!encrypted_batch->TxnBegin()) {
            delete encrypted_batch;
            encrypted_batch = nullptr;
            return false;
        }
        encrypted_batch->WriteMasterKey(nMasterKeyMaxID, kMasterKey);

        for (const auto& spk_man_pair : m_spk_managers) {
            auto spk_man = spk_man_pair.second.get();
            if (!spk_man->Encrypt(_vMasterKey, encrypted_batch)) {
                encrypted_batch->TxnAbort();
                delete encrypted_batch;
                encrypted_batch = nullptr;
                // We now probably have half of our keys encrypted in memory, and half not...
                // die and let the user reload the unencrypted wallet.
                assert(false);
            }
        }

        // Encryption was introduced in version 0.4.0
        SetMinVersion(FEATURE_WALLETCRYPT, encrypted_batch);

        if (!encrypted_batch->TxnCommit()) {
            delete encrypted_batch;
            encrypted_batch = nullptr;
            // We now have keys encrypted in memory, but not on disk...
            // die to avoid confusion and let the user reload the unencrypted wallet.
            assert(false);
        }

        delete encrypted_batch;
        encrypted_batch = nullptr;

        Lock();
        Unlock(strWalletPassphrase);

        // If we are using descriptors, make new descriptors with a new seed
        if (IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS) && !IsWalletFlagSet(WALLET_FLAG_BLANK_WALLET)) {
            SetupDescriptorScriptPubKeyMans();
        } else if (auto spk_man = GetLegacyScriptPubKeyMan()) {
            // if we are using HD, replace the HD seed with a new one
            if (spk_man->IsHDEnabled()) {
                if (!spk_man->SetupGeneration(true)) {
                    return false;
                }
            }
        }
        Lock();

        // Need to completely rewrite the wallet file; if we don't, bdb might keep
        // bits of the unencrypted private key in slack space in the database file.
        GetDatabase().Rewrite();

        // BDB seems to have a bad habit of writing old data into
        // slack space in .dat files; that is bad if the old data is
        // unencrypted private keys. So:
        GetDatabase().ReloadDbEnv();

    }
    NotifyStatusChanged(this);

    return true;
}

DBErrors CWallet::ReorderTransactions()
{
    LOCK(cs_wallet);
    WalletBatch batch(GetDatabase());

    // Old wallets didn't have any defined order for transactions
    // Probably a bad idea to change the output of this

    // First: get all CWalletTx into a sorted-by-time multimap.
    typedef std::multimap<int64_t, CWalletTx*> TxItems;
    TxItems txByTime;

    for (auto& entry : mapWallet)
    {
        CWalletTx* wtx = &entry.second;
        txByTime.insert(std::make_pair(wtx->nTimeReceived, wtx));
    }

    nOrderPosNext = 0;
    std::vector<int64_t> nOrderPosOffsets;
    for (TxItems::iterator it = txByTime.begin(); it != txByTime.end(); ++it)
    {
        CWalletTx *const pwtx = (*it).second;
        int64_t& nOrderPos = pwtx->nOrderPos;

        if (nOrderPos == -1)
        {
            nOrderPos = nOrderPosNext++;
            nOrderPosOffsets.push_back(nOrderPos);

            if (!batch.WriteTx(*pwtx))
                return DBErrors::LOAD_FAIL;
        }
        else
        {
            int64_t nOrderPosOff = 0;
            for (const int64_t& nOffsetStart : nOrderPosOffsets)
            {
                if (nOrderPos >= nOffsetStart)
                    ++nOrderPosOff;
            }
            nOrderPos += nOrderPosOff;
            nOrderPosNext = std::max(nOrderPosNext, nOrderPos + 1);

            if (!nOrderPosOff)
                continue;

            // Since we're changing the order, write it back
            if (!batch.WriteTx(*pwtx))
                return DBErrors::LOAD_FAIL;
        }
    }
    batch.WriteOrderPosNext(nOrderPosNext);

    return DBErrors::LOAD_OK;
}

int64_t CWallet::IncOrderPosNext(WalletBatch* batch)
{
    AssertLockHeld(cs_wallet);
    int64_t nRet = nOrderPosNext++;
    if (batch) {
        batch->WriteOrderPosNext(nOrderPosNext);
    } else {
        WalletBatch(GetDatabase()).WriteOrderPosNext(nOrderPosNext);
    }
    return nRet;
}

void CWallet::MarkDirty()
{
    {
        LOCK(cs_wallet);
        for (std::pair<const uint256, CWalletTx>& item : mapWallet)
            item.second.MarkDirty();
    }
}

bool CWallet::MarkReplaced(const uint256& originalHash, const uint256& newHash)
{
    LOCK(cs_wallet);

    auto mi = mapWallet.find(originalHash);

    // There is a bug if MarkReplaced is not called on an existing wallet transaction.
    assert(mi != mapWallet.end());

    CWalletTx& wtx = (*mi).second;

    // Ensure for now that we're not overwriting data
    assert(wtx.mapValue.count("replaced_by_txid") == 0);

    wtx.mapValue["replaced_by_txid"] = newHash.ToString();

    // Refresh mempool status without waiting for transactionRemovedFromMempool
    // notification so the wallet is in an internally consistent state and
    // immediately knows the old transaction should not be considered trusted
    // and is eligible to be abandoned
    wtx.fInMempool = chain().isInMempool(originalHash);

    WalletBatch batch(GetDatabase());

    bool success = true;
    if (!batch.WriteTx(wtx)) {
        WalletLogPrintf("%s: Updating batch tx %s failed\n", __func__, wtx.GetHash().ToString());
        success = false;
    }

    NotifyTransactionChanged(originalHash, CT_UPDATED);

    return success;
}

void CWallet::SetSpentKeyState(WalletBatch& batch, const uint256& hash, unsigned int n, bool used, std::set<CTxDestination>& tx_destinations)
{
    AssertLockHeld(cs_wallet);
    const CWalletTx* srctx = GetWalletTx(hash);
    if (!srctx) return;

    CTxDestination dst;
    if (ExtractDestination(srctx->tx->vout[n].scriptPubKey, dst)) {
        if (IsMine(dst)) {
            if (used != IsAddressUsed(dst)) {
                if (used) {
                    tx_destinations.insert(dst);
                }
                SetAddressUsed(batch, dst, used);
            }
        }
    }
}

bool CWallet::IsSpentKey(const uint256& hash, unsigned int n) const
{
    AssertLockHeld(cs_wallet);
    const CWalletTx* srctx = GetWalletTx(hash);
    if (srctx) {
        assert(srctx->tx->vout.size() > n);
        CTxDestination dest;
        if (!ExtractDestination(srctx->tx->vout[n].scriptPubKey, dest)) {
            return false;
        }
        if (IsAddressUsed(dest)) {
            return true;
        }
        if (IsLegacy()) {
            LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
            assert(spk_man != nullptr);
            for (const auto& keyid : GetAffectedKeys(srctx->tx->vout[n].scriptPubKey, *spk_man)) {
                WitnessV0KeyHash wpkh_dest(keyid);
                if (IsAddressUsed(wpkh_dest)) {
                    return true;
                }
                ScriptHash sh_wpkh_dest(GetScriptForDestination(wpkh_dest));
                if (IsAddressUsed(sh_wpkh_dest)) {
                    return true;
                }
                PKHash pkh_dest(keyid);
                if (IsAddressUsed(pkh_dest)) {
                    return true;
                }
            }
        }
    }
    return false;
}

CWalletTx* CWallet::AddToWallet(CTransactionRef tx, const CWalletTx::Confirmation& confirm, const UpdateWalletTxFn& update_wtx, bool fFlushOnClose)
{
    LOCK(cs_wallet);

    WalletBatch batch(GetDatabase(), fFlushOnClose);

    uint256 hash = tx->GetHash();

    if (IsWalletFlagSet(WALLET_FLAG_AVOID_REUSE)) {
        // Mark used destinations
        std::set<CTxDestination> tx_destinations;

        for (const CTxIn& txin : tx->vin) {
            const COutPoint& op = txin.prevout;
            SetSpentKeyState(batch, op.hash, op.n, true, tx_destinations);
        }

        MarkDestinationsDirty(tx_destinations);
    }

    // Inserts only if not already there, returns tx inserted or tx found
    auto ret = mapWallet.emplace(std::piecewise_construct, std::forward_as_tuple(hash), std::forward_as_tuple(this, tx));
    CWalletTx& wtx = (*ret.first).second;
    bool fInsertedNew = ret.second;
    bool fUpdated = update_wtx && update_wtx(wtx, fInsertedNew);
    if (fInsertedNew) {
        wtx.m_confirm = confirm;
        wtx.nTimeReceived = chain().getAdjustedTime();
        wtx.nOrderPos = IncOrderPosNext(&batch);
        wtx.m_it_wtxOrdered = wtxOrdered.insert(std::make_pair(wtx.nOrderPos, &wtx));
        wtx.nTimeSmart = ComputeTimeSmart(wtx);
        AddToSpends(hash);
    }

    if (!fInsertedNew)
    {
        if (confirm.status != wtx.m_confirm.status) {
            wtx.m_confirm.status = confirm.status;
            wtx.m_confirm.nIndex = confirm.nIndex;
            wtx.m_confirm.hashBlock = confirm.hashBlock;
            wtx.m_confirm.block_height = confirm.block_height;
            wtx.m_confirm.hasDelegation = confirm.hasDelegation;
            fUpdated = true;
        } else {
            assert(wtx.m_confirm.nIndex == confirm.nIndex);
            assert(wtx.m_confirm.hashBlock == confirm.hashBlock);
            assert(wtx.m_confirm.block_height == confirm.block_height);
            assert(wtx.m_confirm.hasDelegation == confirm.hasDelegation);
        }
        // If we have a witness-stripped version of this transaction, and we
        // see a new version with a witness, then we must be upgrading a pre-segwit
        // wallet.  Store the new version of the transaction with the witness,
        // as the stripped-version must be invalid.
        // TODO: Store all versions of the transaction, instead of just one.
        if (tx->HasWitness() && !wtx.tx->HasWitness()) {
            wtx.SetTx(tx);
            fUpdated = true;
        }
        if(fUpdated && wtx.IsCoinStake())
        {
            AddToSpends(hash);
        }
    }

    // Update unspent addresses
    if(fUpdateAddressUnspentCache)
    {
        std::map<COutPoint, CScriptCache> insertScriptCache;
        for (unsigned int i = 0; i < tx->vout.size(); i++) {
            isminetype mine = IsMine(tx->vout[i]);
            if (!(IsSpent(hash, i)) && mine != ISMINE_NO &&
                !IsLockedCoin(hash, i) && (tx->vout[i].nValue > 0) &&
                // Check if the staking coin is dust
                tx->vout[i].nValue >= m_staker_min_utxo_size)
            {
                // Get the script data for the coin
                COutPoint prevout = COutPoint(hash, i);
                const CScriptCache& scriptCache = GetScriptCache(prevout, tx->vout[i].scriptPubKey, &insertScriptCache);

                // Check that the script is not a contract script
                if(scriptCache.contract || !scriptCache.keyIdOk)
                    continue;

                if(mapAddressUnspentCache.find(scriptCache.keyId) == mapAddressUnspentCache.end())
                {
                    mapAddressUnspentCache[scriptCache.keyId] = true;
                }
            }
        }
    }

    //// debug print
    WalletLogPrintf("AddToWallet %s  %s%s\n", hash.ToString(), (fInsertedNew ? "new" : ""), (fUpdated ? "update" : ""));

    // Write to disk
    if (fInsertedNew || fUpdated)
        if (!batch.WriteTx(wtx))
            return nullptr;

    // Break debit/credit balance caches:
    wtx.MarkDirty();

    // Notify UI of new or updated transaction
    NotifyTransactionChanged(hash, fInsertedNew ? CT_NEW : CT_UPDATED);

#if HAVE_SYSTEM
    // notify an external script when a wallet transaction comes in or is updated
    std::string strCmd = gArgs.GetArg("-walletnotify", "");

    if (!strCmd.empty())
    {
        boost::replace_all(strCmd, "%s", hash.GetHex());
        if (confirm.status == CWalletTx::Status::CONFIRMED)
        {
            boost::replace_all(strCmd, "%b", confirm.hashBlock.GetHex());
            boost::replace_all(strCmd, "%h", ToString(confirm.block_height));
        } else {
            boost::replace_all(strCmd, "%b", "unconfirmed");
            boost::replace_all(strCmd, "%h", "-1");
        }
#ifndef WIN32
        // Substituting the wallet name isn't currently supported on windows
        // because windows shell escaping has not been implemented yet:
        // https://github.com/bitcoin/bitcoin/pull/13339#issuecomment-537384875
        // A few ways it could be implemented in the future are described in:
        // https://github.com/bitcoin/bitcoin/pull/13339#issuecomment-461288094
        boost::replace_all(strCmd, "%w", ShellEscape(GetName()));
#endif
        std::thread t(runCommand, strCmd);
        t.detach(); // thread runs free
    }
#endif

    return &wtx;
}

bool CWallet::LoadToWallet(const uint256& hash, const UpdateWalletTxFn& fill_wtx)
{
    const auto& ins = mapWallet.emplace(std::piecewise_construct, std::forward_as_tuple(hash), std::forward_as_tuple(this, nullptr));
    CWalletTx& wtx = ins.first->second;
    if (!fill_wtx(wtx, ins.second)) {
        return false;
    }
    // If wallet doesn't have a chain (e.g wallet-tool), don't bother to update txn.
    if (HaveChain()) {
        bool active;
        int height;
        bool has_delegation;
        if (chain().findBlock(wtx.m_confirm.hashBlock, FoundBlock().inActiveChain(active).height(height).hasDelegation(has_delegation)) && active) {
            // Update cached block height variable since it not stored in the
            // serialized transaction.
            wtx.m_confirm.block_height = height;
            wtx.m_confirm.hasDelegation = has_delegation;
        } else if (wtx.isConflicted() || wtx.isConfirmed()) {
            // If tx block (or conflicting block) was reorged out of chain
            // while the wallet was shutdown, change tx status to UNCONFIRMED
            // and reset block height, hash, and index. ABANDONED tx don't have
            // associated blocks and don't need to be updated. The case where a
            // transaction was reorged out while online and then reconfirmed
            // while offline is covered by the rescan logic.
            wtx.setUnconfirmed();
            wtx.m_confirm.hashBlock = uint256();
            wtx.m_confirm.block_height = 0;
            wtx.m_confirm.nIndex = 0;
        }
    }
    if (/* insertion took place */ ins.second) {
        wtx.m_it_wtxOrdered = wtxOrdered.insert(std::make_pair(wtx.nOrderPos, &wtx));
    }
    AddToSpends(hash);
    for (const CTxIn& txin : wtx.tx->vin) {
        auto it = mapWallet.find(txin.prevout.hash);
        if (it != mapWallet.end()) {
            CWalletTx& prevtx = it->second;
            if (prevtx.isConflicted()) {
                MarkConflicted(prevtx.m_confirm.hashBlock, prevtx.m_confirm.block_height, wtx.GetHash());
            }
        }
    }
    return true;
}

bool CWallet::AddToWalletIfInvolvingMe(const CTransactionRef& ptx, CWalletTx::Confirmation confirm, bool fUpdate)
{
    const CTransaction& tx = *ptx;
    {
        AssertLockHeld(cs_wallet);

        if (!confirm.hashBlock.IsNull()) {
            for (const CTxIn& txin : tx.vin) {
                std::pair<TxSpends::const_iterator, TxSpends::const_iterator> range = mapTxSpends.equal_range(txin.prevout);
                while (range.first != range.second) {
                    if (range.first->second != tx.GetHash()) {
                        WalletLogPrintf("Transaction %s (in block %s) conflicts with wallet transaction %s (both spend %s:%i)\n", tx.GetHash().ToString(), confirm.hashBlock.ToString(), range.first->second.ToString(), range.first->first.hash.ToString(), range.first->first.n);
                        MarkConflicted(confirm.hashBlock, confirm.block_height, range.first->second);
                    }
                    range.first++;
                }
            }
        }

        bool fExisted = mapWallet.count(tx.GetHash()) != 0;
        if (fExisted && !fUpdate) return false;
        if (fExisted || IsMine(tx) || IsFromMe(tx))
        {
            /* Check if any keys in the wallet keypool that were supposed to be unused
             * have appeared in a new transaction. If so, remove those keys from the keypool.
             * This can happen when restoring an old wallet backup that does not contain
             * the mostly recently created transactions from newer versions of the wallet.
             */

            // loop though all outputs
            for (const CTxOut& txout: tx.vout) {
                for (const auto& spk_man_pair : m_spk_managers) {
                    spk_man_pair.second->MarkUnusedAddresses(txout.scriptPubKey);
                }
            }

            // Block disconnection override an abandoned tx as unconfirmed
            // which means user may have to call abandontransaction again
            return AddToWallet(MakeTransactionRef(tx), confirm, /* update_wtx= */ nullptr, /* fFlushOnClose= */ false);
        }
    }
    return false;
}

bool CWallet::TransactionCanBeAbandoned(const uint256& hashTx) const
{
    LOCK(cs_wallet);
    const CWalletTx* wtx = GetWalletTx(hashTx);
    return wtx && !wtx->isAbandoned() && wtx->GetDepthInMainChain() == 0 && !wtx->InMempool();
}

void CWallet::MarkInputsDirty(const CTransactionRef& tx)
{
    for (const CTxIn& txin : tx->vin) {
        auto it = mapWallet.find(txin.prevout.hash);
        if (it != mapWallet.end()) {
            it->second.MarkDirty();
        }
    }
}

bool CWallet::AbandonTransaction(const uint256& hashTx)
{
    LOCK(cs_wallet);

    WalletBatch batch(GetDatabase());

    std::set<uint256> todo;
    std::set<uint256> done;

    // Can't mark abandoned if confirmed or in mempool
    auto it = mapWallet.find(hashTx);
    assert(it != mapWallet.end());
    const CWalletTx& origtx = it->second;
    if (origtx.GetDepthInMainChain() != 0 || origtx.InMempool()) {
        return false;
    }

    todo.insert(hashTx);

    while (!todo.empty()) {
        uint256 now = *todo.begin();
        todo.erase(now);
        done.insert(now);
        auto it = mapWallet.find(now);
        assert(it != mapWallet.end());
        CWalletTx& wtx = it->second;
        int currentconfirm = wtx.GetDepthInMainChain();
        // If the orig tx was not in block, none of its spends can be
        assert(currentconfirm <= 0);
        // if (currentconfirm < 0) {Tx and spends are already conflicted, no need to abandon}
        if (currentconfirm == 0 && !wtx.isAbandoned()) {
            // If the orig tx was not in block/mempool, none of its spends can be in mempool
            assert(!wtx.InMempool());
            wtx.setAbandoned();
            wtx.MarkDirty();
            batch.WriteTx(wtx);
            NotifyTransactionChanged(wtx.GetHash(), CT_UPDATED);
            // Iterate over all its outputs, and mark transactions in the wallet that spend them abandoned too
            TxSpends::const_iterator iter = mapTxSpends.lower_bound(COutPoint(now, 0));
            while (iter != mapTxSpends.end() && iter->first.hash == now) {
                if (!done.count(iter->second)) {
                    todo.insert(iter->second);
                }
                iter++;
            }
            // If a transaction changes 'conflicted' state, that changes the balance
            // available of the outputs it spends. So force those to be recomputed
            MarkInputsDirty(wtx.tx);
        }
    }

    return true;
}

void CWallet::MarkConflicted(const uint256& hashBlock, int conflicting_height, const uint256& hashTx)
{
    LOCK(cs_wallet);

    int conflictconfirms = (m_last_block_processed_height - conflicting_height + 1) * -1;
    // If number of conflict confirms cannot be determined, this means
    // that the block is still unknown or not yet part of the main chain,
    // for example when loading the wallet during a reindex. Do nothing in that
    // case.
    if (conflictconfirms >= 0)
        return;

    // Do not flush the wallet here for performance reasons
    WalletBatch batch(GetDatabase(), false);

    std::set<uint256> todo;
    std::set<uint256> done;

    todo.insert(hashTx);

    while (!todo.empty()) {
        uint256 now = *todo.begin();
        todo.erase(now);
        done.insert(now);
        auto it = mapWallet.find(now);
        assert(it != mapWallet.end());
        CWalletTx& wtx = it->second;
        int currentconfirm = wtx.GetDepthInMainChain();
        if (conflictconfirms < currentconfirm) {
            // Block is 'more conflicted' than current confirm; update.
            // Mark transaction as conflicted with this block.
            wtx.m_confirm.nIndex = 0;
            wtx.m_confirm.hashBlock = hashBlock;
            wtx.m_confirm.block_height = conflicting_height;
            wtx.setConflicted();
            wtx.MarkDirty();
            batch.WriteTx(wtx);
            // Iterate over all its outputs, and mark transactions in the wallet that spend them conflicted too
            TxSpends::const_iterator iter = mapTxSpends.lower_bound(COutPoint(now, 0));
            while (iter != mapTxSpends.end() && iter->first.hash == now) {
                 if (!done.count(iter->second)) {
                     todo.insert(iter->second);
                 }
                 iter++;
            }
            // If a transaction changes 'conflicted' state, that changes the balance
            // available of the outputs it spends. So force those to be recomputed
            MarkInputsDirty(wtx.tx);
        }
    }
}

void CWallet::SyncTransaction(const CTransactionRef& ptx, CWalletTx::Confirmation confirm, bool update_tx)
{
    if (confirm.hashBlock.IsNull() && confirm.nIndex == -1)
    {
        // wallets need to refund inputs when disconnecting coinstake
        const CTransaction& tx = *ptx;
        if (tx.IsCoinStake() && IsFromMe(tx))
        {
            DisableTransaction(tx);
            return;
        }
    }

    if (!AddToWalletIfInvolvingMe(ptx, confirm, update_tx))
        return; // Not one of ours

    // If a transaction changes 'conflicted' state, that changes the balance
    // available of the outputs it spends. So force those to be
    // recomputed, also:
    MarkInputsDirty(ptx);
}

void CWallet::transactionAddedToMempool(const CTransactionRef& tx, uint64_t mempool_sequence) {
    LOCK(cs_wallet);
    SyncTransaction(tx, {CWalletTx::Status::UNCONFIRMED, /* block height */ 0, /* block hash */ {}, /* index */ 0, /* hasDelegation */ false});

    auto it = mapWallet.find(tx->GetHash());
    if (it != mapWallet.end()) {
        it->second.fInMempool = true;
    }
}

void CWallet::transactionRemovedFromMempool(const CTransactionRef& tx, MemPoolRemovalReason reason, uint64_t mempool_sequence) {
    LOCK(cs_wallet);
    auto it = mapWallet.find(tx->GetHash());
    if (it != mapWallet.end()) {
        it->second.fInMempool = false;
    }
    // Handle transactions that were removed from the mempool because they
    // conflict with transactions in a newly connected block.
    if (reason == MemPoolRemovalReason::CONFLICT) {
        // Trigger external -walletnotify notifications for these transactions.
        // Set Status::UNCONFIRMED instead of Status::CONFLICTED for a few reasons:
        //
        // 1. The transactionRemovedFromMempool callback does not currently
        //    provide the conflicting block's hash and height, and for backwards
        //    compatibility reasons it may not be not safe to store conflicted
        //    wallet transactions with a null block hash. See
        //    https://github.com/bitcoin/bitcoin/pull/18600#discussion_r420195993.
        // 2. For most of these transactions, the wallet's internal conflict
        //    detection in the blockConnected handler will subsequently call
        //    MarkConflicted and update them with CONFLICTED status anyway. This
        //    applies to any wallet transaction that has inputs spent in the
        //    block, or that has ancestors in the wallet with inputs spent by
        //    the block.
        // 3. Longstanding behavior since the sync implementation in
        //    https://github.com/bitcoin/bitcoin/pull/9371 and the prior sync
        //    implementation before that was to mark these transactions
        //    unconfirmed rather than conflicted.
        //
        // Nothing described above should be seen as an unchangeable requirement
        // when improving this code in the future. The wallet's heuristics for
        // distinguishing between conflicted and unconfirmed transactions are
        // imperfect, and could be improved in general, see
        // https://github.com/bitcoin-core/bitcoin-devwiki/wiki/Wallet-Transaction-Conflict-Tracking
        SyncTransaction(tx, {CWalletTx::Status::UNCONFIRMED, /* block height */ 0, /* block hash */ {}, /* index */ 0, /* hasDelegation */ false});
    }
}

void CWallet::blockConnected(const CBlock& block, int height)
{
    const uint256& block_hash = block.GetHash();
    bool hasDelegation = block.HasProofOfDelegation();
    LOCK(cs_wallet);

    m_last_block_processed_height = height;
    m_last_block_processed = block_hash;
    for (size_t index = 0; index < block.vtx.size(); index++) {
        SyncTransaction(block.vtx[index], {CWalletTx::Status::CONFIRMED, height, block_hash, (int)index, hasDelegation});
        transactionRemovedFromMempool(block.vtx[index], MemPoolRemovalReason::BLOCK, 0 /* mempool_sequence */);
    }
}

void CWallet::blockDisconnected(const CBlock& block, int height)
{
    LOCK(cs_wallet);

    // At block disconnection, this will change an abandoned transaction to
    // be unconfirmed, whether or not the transaction is added back to the mempool.
    // User may have to call abandontransaction again. It may be addressed in the
    // future with a stickier abandoned state or even removing abandontransaction call.
    m_last_block_processed_height = height - 1;
    m_last_block_processed = block.hashPrevBlock;
    for (const CTransactionRef& ptx : block.vtx) {
        int index = ptx->IsCoinStake() ? -1 : 0;
        SyncTransaction(ptx, {CWalletTx::Status::UNCONFIRMED, /* block height */ 0, /* block hash */ {}, index, /* hasDelegation */ false});
    }
}

void CWallet::updatedBlockTip()
{
    m_best_block_time = GetTime();
}


void CWallet::BlockUntilSyncedToCurrentChain() const {
    AssertLockNotHeld(cs_wallet);
    // Skip the queue-draining stuff if we know we're caught up with
    // ::ChainActive().Tip(), otherwise put a callback in the validation interface queue and wait
    // for the queue to drain enough to execute it (indicating we are caught up
    // at least with the time we entered this function).
    uint256 last_block_hash = WITH_LOCK(cs_wallet, return m_last_block_processed);
    chain().waitForNotificationsIfTipChanged(last_block_hash);
}

// Note that this function doesn't distinguish between a 0-valued input,
// and a not-"is mine" (according to the filter) input.
CAmount CWallet::GetDebit(const CTxIn &txin, const isminefilter& filter) const
{
    {
        LOCK(cs_wallet);
        std::map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(txin.prevout.hash);
        if (mi != mapWallet.end())
        {
            const CWalletTx& prev = (*mi).second;
            if (txin.prevout.n < prev.tx->vout.size())
                if (IsMine(prev.tx->vout[txin.prevout.n]) & filter)
                    return prev.tx->vout[txin.prevout.n].nValue;
        }
    }
    return 0;
}

isminetype CWallet::IsMine(const CTxOut& txout) const
{
    return IsMine(txout.scriptPubKey);
}

isminetype CWallet::IsMine(const CTxDestination& dest) const
{
    AssertLockHeld(cs_wallet);
    return IsMine(GetScriptForDestination(dest));
}

isminetype CWallet::IsMine(const CScript& script) const
{
    isminetype result = ISMINE_NO;
    for (const auto& spk_man_pair : m_spk_managers) {
        result = std::max(result, spk_man_pair.second->IsMine(script));
    }
    return result;
}

bool CWallet::IsMine(const CTransaction& tx) const
{
    AssertLockHeld(cs_wallet);
    for (const CTxOut& txout : tx.vout)
        if (IsMine(txout))
            return true;
    return false;
}

bool CWallet::IsFromMe(const CTransaction& tx) const
{
    return (GetDebit(tx, ISMINE_ALL) > 0);
}

CAmount CWallet::GetDebit(const CTransaction& tx, const isminefilter& filter) const
{
    CAmount nDebit = 0;
    for (const CTxIn& txin : tx.vin)
    {
        nDebit += GetDebit(txin, filter);
        if (!MoneyRange(nDebit))
            throw std::runtime_error(std::string(__func__) + ": value out of range");
    }
    return nDebit;
}

bool CWallet::IsHDEnabled() const
{
    // All Active ScriptPubKeyMans must be HD for this to be true
    bool result = true;
    for (const auto& spk_man : GetActiveScriptPubKeyMans()) {
        result &= spk_man->IsHDEnabled();
    }
    return result;
}

bool CWallet::CanGetAddresses(bool internal) const
{
    LOCK(cs_wallet);
    if (m_spk_managers.empty()) return false;
    for (OutputType t : OUTPUT_TYPES) {
        auto spk_man = GetScriptPubKeyMan(t, internal);
        if (spk_man && spk_man->CanGetAddresses(internal)) {
            return true;
        }
    }
    return false;
}

void CWallet::SetWalletFlag(uint64_t flags)
{
    LOCK(cs_wallet);
    m_wallet_flags |= flags;
    if (!WalletBatch(GetDatabase()).WriteWalletFlags(m_wallet_flags))
        throw std::runtime_error(std::string(__func__) + ": writing wallet flags failed");
}

void CWallet::UnsetWalletFlag(uint64_t flag)
{
    WalletBatch batch(GetDatabase());
    UnsetWalletFlagWithDB(batch, flag);
}

void CWallet::UnsetWalletFlagWithDB(WalletBatch& batch, uint64_t flag)
{
    LOCK(cs_wallet);
    m_wallet_flags &= ~flag;
    if (!batch.WriteWalletFlags(m_wallet_flags))
        throw std::runtime_error(std::string(__func__) + ": writing wallet flags failed");
}

void CWallet::UnsetBlankWalletFlag(WalletBatch& batch)
{
    UnsetWalletFlagWithDB(batch, WALLET_FLAG_BLANK_WALLET);
}

bool CWallet::IsWalletFlagSet(uint64_t flag) const
{
    return (m_wallet_flags & flag);
}

bool CWallet::LoadWalletFlags(uint64_t flags)
{
    LOCK(cs_wallet);
    if (((flags & KNOWN_WALLET_FLAGS) >> 32) ^ (flags >> 32)) {
        // contains unknown non-tolerable wallet flags
        return false;
    }
    m_wallet_flags = flags;

    return true;
}

bool CWallet::AddWalletFlags(uint64_t flags)
{
    LOCK(cs_wallet);
    // We should never be writing unknown non-tolerable wallet flags
    assert(((flags & KNOWN_WALLET_FLAGS) >> 32) == (flags >> 32));
    if (!WalletBatch(GetDatabase()).WriteWalletFlags(flags)) {
        throw std::runtime_error(std::string(__func__) + ": writing wallet flags failed");
    }

    return LoadWalletFlags(flags);
}

// Helper for producing a max-sized low-S low-R signature (eg 71 bytes)
// or a max-sized low-S signature (e.g. 72 bytes) if use_max_sig is true
bool CWallet::DummySignInput(CTxIn &tx_in, const CTxOut &txout, bool use_max_sig) const
{
    // Fill in dummy signatures for fee calculation.
    const CScript& scriptPubKey = txout.scriptPubKey;
    SignatureData sigdata;

    std::unique_ptr<SigningProvider> provider = GetSolvingProvider(scriptPubKey);
    if (!provider) {
        // We don't know about this scriptpbuKey;
        return false;
    }

    if (!ProduceSignature(*provider, use_max_sig ? DUMMY_MAXIMUM_SIGNATURE_CREATOR : DUMMY_SIGNATURE_CREATOR, scriptPubKey, sigdata)) {
        return false;
    }
    UpdateInput(tx_in, sigdata);
    return true;
}

// Helper for producing a bunch of max-sized low-S low-R signatures (eg 71 bytes)
bool CWallet::DummySignTx(CMutableTransaction &txNew, const std::vector<CTxOut> &txouts, bool use_max_sig) const
{
    // Fill in dummy signatures for fee calculation.
    int nIn = 0;
    for (const auto& txout : txouts)
    {
        if (!DummySignInput(txNew.vin[nIn], txout, use_max_sig)) {
            return false;
        }

        nIn++;
    }
    return true;
}

bool CWallet::ImportScripts(const std::set<CScript> scripts, int64_t timestamp)
{
    auto spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        return false;
    }
    LOCK(spk_man->cs_KeyStore);
    return spk_man->ImportScripts(scripts, timestamp);
}

bool CWallet::ImportPrivKeys(const std::map<CKeyID, CKey>& privkey_map, const int64_t timestamp)
{
    auto spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        return false;
    }
    LOCK(spk_man->cs_KeyStore);
    return spk_man->ImportPrivKeys(privkey_map, timestamp);
}

bool CWallet::ImportPubKeys(const std::vector<CKeyID>& ordered_pubkeys, const std::map<CKeyID, CPubKey>& pubkey_map, const std::map<CKeyID, std::pair<CPubKey, KeyOriginInfo>>& key_origins, const bool add_keypool, const bool internal, const int64_t timestamp)
{
    auto spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        return false;
    }
    LOCK(spk_man->cs_KeyStore);
    return spk_man->ImportPubKeys(ordered_pubkeys, pubkey_map, key_origins, add_keypool, internal, timestamp);
}

bool CWallet::ImportScriptPubKeys(const std::string& label, const std::set<CScript>& script_pub_keys, const bool have_solving_data, const bool apply_label, const int64_t timestamp)
{
    auto spk_man = GetLegacyScriptPubKeyMan();
    if (!spk_man) {
        return false;
    }
    LOCK(spk_man->cs_KeyStore);
    if (!spk_man->ImportScriptPubKeys(script_pub_keys, have_solving_data, timestamp)) {
        return false;
    }
    if (apply_label) {
        WalletBatch batch(GetDatabase());
        for (const CScript& script : script_pub_keys) {
            CTxDestination dest;
            ExtractDestination(script, dest);
            if (IsValidDestination(dest)) {
                SetAddressBookWithDB(batch, dest, label, "receive");
            }
        }
    }
    return true;
}

/**
 * Scan active chain for relevant transactions after importing keys. This should
 * be called whenever new keys are added to the wallet, with the oldest key
 * creation time.
 *
 * @return Earliest timestamp that could be successfully scanned from. Timestamp
 * returned will be higher than startTime if relevant blocks could not be read.
 */
int64_t CWallet::RescanFromTime(int64_t startTime, const WalletRescanReserver& reserver, bool update)
{
    // Find starting block. May be null if nCreateTime is greater than the
    // highest blockchain timestamp, in which case there is nothing that needs
    // to be scanned.
    int start_height = 0;
    uint256 start_block;
    bool start = chain().findFirstBlockWithTimeAndHeight(startTime - TIMESTAMP_WINDOW, 0, FoundBlock().hash(start_block).height(start_height));
    WalletLogPrintf("%s: Rescanning last %i blocks\n", __func__, start ? WITH_LOCK(cs_wallet, return GetLastBlockHeight()) - start_height + 1 : 0);

    if (start) {
        // TODO: this should take into account failure by ScanResult::USER_ABORT
        ScanResult result = ScanForWalletTransactions(start_block, start_height, {} /* max_height */, reserver, update);
        if (result.status == ScanResult::FAILURE) {
            int64_t time_max;
            CHECK_NONFATAL(chain().findBlock(result.last_failed_block, FoundBlock().maxTime(time_max)));
            return time_max + TIMESTAMP_WINDOW + 1;
        }
    }
    return startTime;
}

/**
 * Scan the block chain (starting in start_block) for transactions
 * from or to us. If fUpdate is true, found transactions that already
 * exist in the wallet will be updated.
 *
 * @param[in] start_block Scan starting block. If block is not on the active
 *                        chain, the scan will return SUCCESS immediately.
 * @param[in] start_height Height of start_block
 * @param[in] max_height  Optional max scanning height. If unset there is
 *                        no maximum and scanning can continue to the tip
 *
 * @return ScanResult returning scan information and indicating success or
 *         failure. Return status will be set to SUCCESS if scan was
 *         successful. FAILURE if a complete rescan was not possible (due to
 *         pruning or corruption). USER_ABORT if the rescan was aborted before
 *         it could complete.
 *
 * @pre Caller needs to make sure start_block (and the optional stop_block) are on
 * the main chain after to the addition of any new keys you want to detect
 * transactions for.
 */
CWallet::ScanResult CWallet::ScanForWalletTransactions(const uint256& start_block, int start_height, std::optional<int> max_height, const WalletRescanReserver& reserver, bool fUpdate)
{
    int64_t nNow = GetTime();
    int64_t start_time = GetTimeMillis();

    assert(reserver.isReserved());

    uint256 block_hash = start_block;
    ScanResult result;

    WalletLogPrintf("Rescan started from block %s...\n", start_block.ToString());

    fAbortRescan = false;
    ShowProgress(strprintf("%s " + _("Rescanning…").translated, GetDisplayName()), 0); // show rescan progress in GUI as dialog or on splashscreen, if -rescan on startup
    uint256 tip_hash = WITH_LOCK(cs_wallet, return GetLastBlockHash());
    uint256 end_hash = tip_hash;
    if (max_height) chain().findAncestorByHeight(tip_hash, *max_height, FoundBlock().hash(end_hash));
    double progress_begin = chain().guessVerificationProgress(block_hash);
    double progress_end = chain().guessVerificationProgress(end_hash);
    double progress_current = progress_begin;
    int block_height = start_height;
    while (!fAbortRescan && !chain().shutdownRequested()) {
        if (progress_end - progress_begin > 0.0) {
            m_scanning_progress = (progress_current - progress_begin) / (progress_end - progress_begin);
        } else { // avoid divide-by-zero for single block scan range (i.e. start and stop hashes are equal)
            m_scanning_progress = 0;
        }
        if (block_height % 100 == 0 && progress_end - progress_begin > 0.0) {
            ShowProgress(strprintf("%s " + _("Rescanning…").translated, GetDisplayName()), std::max(1, std::min(99, (int)(m_scanning_progress * 100))));
        }
        if (GetTime() >= nNow + 60) {
            nNow = GetTime();
            WalletLogPrintf("Still rescanning. At block %d. Progress=%f\n", block_height, progress_current);
        }

        // Read block data
        CBlock block;
        chain().findBlock(block_hash, FoundBlock().data(block));

        // Find next block separately from reading data above, because reading
        // is slow and there might be a reorg while it is read.
        bool block_still_active = false;
        bool next_block = false;
        uint256 next_block_hash;
        chain().findBlock(block_hash, FoundBlock().inActiveChain(block_still_active).nextBlock(FoundBlock().inActiveChain(next_block).hash(next_block_hash)));

        if (!block.IsNull()) {
            LOCK(cs_wallet);
            if (!block_still_active) {
                // Abort scan if current block is no longer active, to prevent
                // marking transactions as coming from the wrong block.
                result.last_failed_block = block_hash;
                result.status = ScanResult::FAILURE;
                break;
            }
            bool hasDelegation = block.HasProofOfDelegation();
            for (size_t posInBlock = 0; posInBlock < block.vtx.size(); ++posInBlock) {
                SyncTransaction(block.vtx[posInBlock], {CWalletTx::Status::CONFIRMED, block_height, block_hash, (int)posInBlock, hasDelegation}, fUpdate);
            }
            // scan succeeded, record block as most recent successfully scanned
            result.last_scanned_block = block_hash;
            result.last_scanned_height = block_height;
        } else {
            // could not scan block, keep scanning but record this block as the most recent failure
            result.last_failed_block = block_hash;
            result.status = ScanResult::FAILURE;
        }
        if (max_height && block_height >= *max_height) {
            break;
        }
        {
            if (!next_block) {
                // break successfully when rescan has reached the tip, or
                // previous block is no longer on the chain due to a reorg
                break;
            }

            // increment block and verification progress
            block_hash = next_block_hash;
            ++block_height;
            progress_current = chain().guessVerificationProgress(block_hash);

            // handle updated tip hash
            const uint256 prev_tip_hash = tip_hash;
            tip_hash = WITH_LOCK(cs_wallet, return GetLastBlockHash());
            if (!max_height && prev_tip_hash != tip_hash) {
                // in case the tip has changed, update progress max
                progress_end = chain().guessVerificationProgress(tip_hash);
            }
        }
    }
    ShowProgress(strprintf("%s " + _("Rescanning…").translated, GetDisplayName()), 100); // hide progress dialog in GUI
    if (block_height && fAbortRescan) {
        WalletLogPrintf("Rescan aborted at block %d. Progress=%f\n", block_height, progress_current);
        result.status = ScanResult::USER_ABORT;
    } else if (block_height && chain().shutdownRequested()) {
        WalletLogPrintf("Rescan interrupted by shutdown request at block %d. Progress=%f\n", block_height, progress_current);
        result.status = ScanResult::USER_ABORT;
    } else {
        WalletLogPrintf("Rescan completed in %15dms\n", GetTimeMillis() - start_time);
    }
    return result;
}

void CWallet::ReacceptWalletTransactions()
{
    // If transactions aren't being broadcasted, don't let them into local mempool either
    if (!fBroadcastTransactions)
        return;
    std::map<int64_t, CWalletTx*> mapSorted;

    // Sort pending wallet transactions based on their initial wallet insertion order
    for (std::pair<const uint256, CWalletTx>& item : mapWallet) {
        const uint256& wtxid = item.first;
        CWalletTx& wtx = item.second;
        assert(wtx.GetHash() == wtxid);

        int nDepth = wtx.GetDepthInMainChain();

        if (!(wtx.IsCoinBase() || wtx.IsCoinStake()) && (nDepth == 0 && !wtx.isAbandoned())) {
            mapSorted.insert(std::make_pair(wtx.nOrderPos, &wtx));
        }
    }

    // Try to add wallet transactions to memory pool
    for (const std::pair<const int64_t, CWalletTx*>& item : mapSorted) {
        CWalletTx& wtx = *(item.second);
        std::string unused_err_string;
        wtx.SubmitMemoryPoolAndRelay(unused_err_string, false);
    }
}

bool CWalletTx::SubmitMemoryPoolAndRelay(std::string& err_string, bool relay)
{
    // Can't relay if wallet is not broadcasting
    if (!pwallet->GetBroadcastTransactions()) return false;
    // Don't relay abandoned transactions
    if (isAbandoned()) return false;
    // Don't try to submit coinbase transactions. These would fail anyway but would
    // cause log spam.
    if (IsCoinBase() || IsCoinStake()) return false;
    // Don't try to submit conflicted or confirmed transactions.
    if (GetDepthInMainChain() != 0) return false;

    // Submit transaction to mempool for relay
    pwallet->WalletLogPrintf("Submitting wtx %s to mempool for relay\n", GetHash().ToString());
    // We must set fInMempool here - while it will be re-set to true by the
    // entered-mempool callback, if we did not there would be a race where a
    // user could call sendmoney in a loop and hit spurious out of funds errors
    // because we think that this newly generated transaction's change is
    // unavailable as we're not yet aware that it is in the mempool.
    //
    // Irrespective of the failure reason, un-marking fInMempool
    // out-of-order is incorrect - it should be unmarked when
    // TransactionRemovedFromMempool fires.
    bool ret = pwallet->chain().broadcastTransaction(tx, pwallet->m_default_max_tx_fee, relay, err_string);
    fInMempool |= ret;
    return ret;
}

std::set<uint256> CWalletTx::GetConflicts() const
{
    std::set<uint256> result;
    if (pwallet != nullptr)
    {
        uint256 myHash = GetHash();
        result = pwallet->GetConflicts(myHash);
        result.erase(myHash);
    }
    return result;
}

// Rebroadcast transactions from the wallet. We do this on a random timer
// to slightly obfuscate which transactions come from our wallet.
//
// Ideally, we'd only resend transactions that we think should have been
// mined in the most recent block. Any transaction that wasn't in the top
// blockweight of transactions in the mempool shouldn't have been mined,
// and so is probably just sitting in the mempool waiting to be confirmed.
// Rebroadcasting does nothing to speed up confirmation and only damages
// privacy.
void CWallet::ResendWalletTransactions()
{
    // During reindex, importing and IBD, old wallet transactions become
    // unconfirmed. Don't resend them as that would spam other nodes.
    if (!chain().isReadyToBroadcast()) return;

    // Do this infrequently and randomly to avoid giving away
    // that these are our transactions.
    if (GetTime() < nNextResend || !fBroadcastTransactions) return;
    bool fFirst = (nNextResend == 0);
    // resend 12-36 hours from now, ~1 day on average.
    nNextResend = GetTime() + (12 * 60 * 60) + GetRand(24 * 60 * 60);
    if (fFirst) return;

    int submitted_tx_count = 0;

    { // cs_wallet scope
        LOCK(cs_wallet);

        // Relay transactions
        for (std::pair<const uint256, CWalletTx>& item : mapWallet) {
            CWalletTx& wtx = item.second;
            // Attempt to rebroadcast all txes more than 5 minutes older than
            // the last block. SubmitMemoryPoolAndRelay() will not rebroadcast
            // any confirmed or conflicting txs.
            if (wtx.nTimeReceived > m_best_block_time - 5 * 60) continue;
            std::string unused_err_string;
            if (wtx.SubmitMemoryPoolAndRelay(unused_err_string, true)) ++submitted_tx_count;
        }
    } // cs_wallet

    if (submitted_tx_count > 0) {
        WalletLogPrintf("%s: resubmit %u unconfirmed transactions\n", __func__, submitted_tx_count);
    }
}

/** @} */ // end of mapWallet

void MaybeResendWalletTxs()
{
    for (const std::shared_ptr<CWallet>& pwallet : GetWallets()) {
        pwallet->ResendWalletTransactions();
    }
}


/** @defgroup Actions
 *
 * @{
 */

const CScriptCache& CWallet::GetScriptCache(const COutPoint& prevout, const CScript& scriptPubKey, std::map<COutPoint, CScriptCache>* _insertScriptCache) const
{
    auto it = prevoutScriptCache.find(prevout);
    if(it == prevoutScriptCache.end())
    {
        std::map<COutPoint, CScriptCache>& insertScriptCache = _insertScriptCache == nullptr ? prevoutScriptCache : *_insertScriptCache;
        if((int32_t)insertScriptCache.size() > m_staker_max_utxo_script_cache)
        {
            insertScriptCache.clear();
        }

        // The script check for utxo is expensive operations, so cache the data for further use
        CScriptCache scriptCache;
        scriptCache.contract = scriptPubKey.HasOpCall() || scriptPubKey.HasOpCreate();
        if(!scriptCache.contract)
        {
            scriptCache.keyId = ToKeyID(ExtractPublicKeyHash(scriptPubKey, &(scriptCache.keyIdOk)));
            if(scriptCache.keyIdOk)
            {
                std::unique_ptr<SigningProvider> provider = GetSolvingProvider(scriptPubKey);
                scriptCache.solvable = provider ? IsSolvable(*provider, scriptPubKey) : false;
            }
        }
        insertScriptCache[prevout] = scriptCache;
        return insertScriptCache[prevout];
    }

    return it->second;
}

bool valueUtxoSort(const std::pair<COutPoint,CAmount>& a,
                const std::pair<COutPoint,CAmount>& b) {
    return a.second > b.second;
}

bool CWallet::SignTransaction(CMutableTransaction& tx) const
{
    AssertLockHeld(cs_wallet);

    // Build coins map
    std::map<COutPoint, Coin> coins;
    for (auto& input : tx.vin) {
        std::map<uint256, CWalletTx>::const_iterator mi = mapWallet.find(input.prevout.hash);
        if(mi == mapWallet.end() || input.prevout.n >= mi->second.tx->vout.size()) {
            return false;
        }
        const CWalletTx& wtx = mi->second;
        coins[input.prevout] = Coin(wtx.tx->vout[input.prevout.n], wtx.m_confirm.block_height, wtx.IsCoinBase(), wtx.IsCoinStake());
    }
    std::map<int, std::string> input_errors;
    return SignTransaction(tx, coins, SIGHASH_DEFAULT, input_errors);
}

bool CWallet::SignTransaction(CMutableTransaction& tx, const std::map<COutPoint, Coin>& coins, int sighash, std::map<int, std::string>& input_errors) const
{
    // Try to sign with all ScriptPubKeyMans
    for (ScriptPubKeyMan* spk_man : GetAllScriptPubKeyMans()) {
        // spk_man->SignTransaction will return true if the transaction is complete,
        // so we can exit early and return true if that happens
        if (spk_man->SignTransaction(tx, coins, sighash, input_errors)) {
            return true;
        }
    }

    // At this point, one input was not fully signed otherwise we would have exited already
    return false;
}

bool CWallet::SignTransactionOutput(CMutableTransaction& tx) const
{
    std::map<int, std::string> output_errors;
    return SignTransactionOutput(tx, SIGHASH_ALL, output_errors);
}

bool CWallet::SignTransactionOutput(CMutableTransaction& tx, int sighash, std::map<int, std::string>& output_errors) const
{
    // Sign transaction op_sender outputs
    for (ScriptPubKeyMan* spk_man : GetAllScriptPubKeyMans()) {
        if (spk_man->SignTransactionOutput(tx, sighash, output_errors)) {
            return true;
        }
    }

    return false;
}

bool CWallet::SignTransactionStake(CMutableTransaction& txTo, const std::vector<std::pair<const CWalletTx*,unsigned int>>& vwtxPrev) const
{
    // Create the list of coins
    std::vector<std::pair<CTxOut,unsigned int>> coins;
    unsigned int nIn = 0;
    for(const std::pair<const CWalletTx*,unsigned int> &pcoin : vwtxPrev)
    {
        const CTransaction& txFrom = *pcoin.first->tx;
        assert(nIn < txTo.vin.size());
        CTxIn& txin = txTo.vin[nIn];
        assert(txin.prevout.n < txFrom.vout.size());
        const CTxOut& txout = txFrom.vout[txin.prevout.n];
        coins.push_back(std::make_pair(txout, nIn));
        nIn++;
    }

    // Sign coinstake transaction
    for (ScriptPubKeyMan* spk_man : GetAllScriptPubKeyMans()) {
        if (spk_man->SignTransactionStake(txTo, coins)) {
            return true;
        }
    }

    return false;
}

bool CWallet::SignBlockStake(CBlock& block, const PKHash& pkhash, bool compact) const
{
    // Sign coinstake transaction
    for (ScriptPubKeyMan* spk_man : GetAllScriptPubKeyMans()) {
        if (spk_man->SignBlockStake(block, pkhash, compact)) {
            return true;
        }
    }

    return false;
}


TransactionError CWallet::FillPSBT(PartiallySignedTransaction& psbtx, bool& complete, int sighash_type, bool sign, bool bip32derivs, size_t * n_signed) const
{
    if (n_signed) {
        *n_signed = 0;
    }
    const PrecomputedTransactionData txdata = PrecomputePSBTData(psbtx);
    LOCK(cs_wallet);
    // Get all of the previous transactions
    for (unsigned int i = 0; i < psbtx.tx->vin.size(); ++i) {
        const CTxIn& txin = psbtx.tx->vin[i];
        PSBTInput& input = psbtx.inputs.at(i);

        if (PSBTInputSigned(input)) {
            continue;
        }

        // If we have no utxo, grab it from the wallet.
        if (!input.non_witness_utxo) {
            const uint256& txhash = txin.prevout.hash;
            const auto it = mapWallet.find(txhash);
            if (it != mapWallet.end()) {
                const CWalletTx& wtx = it->second;
                // We only need the non_witness_utxo, which is a superset of the witness_utxo.
                //   The signing code will switch to the smaller witness_utxo if this is ok.
                input.non_witness_utxo = wtx.tx;
            }
        }
    }

    // Fill in information from ScriptPubKeyMans
    for (ScriptPubKeyMan* spk_man : GetAllScriptPubKeyMans()) {
        int n_signed_this_spkm = 0;
        TransactionError res = spk_man->FillPSBT(psbtx, txdata, sighash_type, sign, bip32derivs, &n_signed_this_spkm);
        if (res != TransactionError::OK) {
            return res;
        }

        if (n_signed) {
            (*n_signed) += n_signed_this_spkm;
        }
    }

    // Complete if every input is now signed
    complete = true;
    for (const auto& input : psbtx.inputs) {
        complete &= PSBTInputSigned(input);
    }

    return TransactionError::OK;
}

SigningResult CWallet::SignMessage(const std::string& message, const PKHash& pkhash, std::string& str_sig) const
{
    SignatureData sigdata;
    CScript script_pub_key = GetScriptForDestination(pkhash);
    for (const auto& spk_man_pair : m_spk_managers) {
        if (spk_man_pair.second->CanProvide(script_pub_key, sigdata)) {
            return spk_man_pair.second->SignMessage(message, pkhash, str_sig);
        }
    }
    return SigningResult::PRIVATE_KEY_NOT_AVAILABLE;
}

OutputType CWallet::TransactionChangeType(const std::optional<OutputType>& change_type, const std::vector<CRecipient>& vecSend) const
{
    // If -changetype is specified, always use that change type.
    if (change_type) {
        return *change_type;
    }

    // if m_default_address_type is legacy, use legacy address as change (even
    // if some of the outputs are P2WPKH or P2WSH).
    if (m_default_address_type == OutputType::LEGACY) {
        return OutputType::LEGACY;
    }

    // if any destination is P2WPKH or P2WSH, use P2WPKH for the change
    // output.
    for (const auto& recipient : vecSend) {
        // Check if any destination contains a witness program:
        int witnessversion = 0;
        std::vector<unsigned char> witnessprogram;
        if (recipient.scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram)) {
            if (GetScriptPubKeyMan(OutputType::BECH32M, true)) {
                return OutputType::BECH32M;
            } else if (GetScriptPubKeyMan(OutputType::BECH32, true)) {
                return OutputType::BECH32;
            } else {
                return m_default_address_type;
            }
        }
    }

    // else use m_default_address_type for change
    return m_default_address_type;
}

void CWallet::CommitTransaction(CTransactionRef tx, mapValue_t mapValue, std::vector<std::pair<std::string, std::string>> orderForm)
{
    LOCK(cs_wallet);
    WalletLogPrintf("CommitTransaction:\n%s", tx->ToString()); /* Continued */

    // Add tx to wallet, because if it has change it's also ours,
    // otherwise just for transaction history.
    AddToWallet(tx, {}, [&](CWalletTx& wtx, bool new_tx) {
        CHECK_NONFATAL(wtx.mapValue.empty());
        CHECK_NONFATAL(wtx.vOrderForm.empty());
        wtx.mapValue = std::move(mapValue);
        wtx.vOrderForm = std::move(orderForm);
        wtx.fTimeReceivedIsTxTime = true;
        wtx.fFromMe = true;
        return true;
    });

    // Notify that old coins are spent
    for (const CTxIn& txin : tx->vin) {
        CWalletTx &coin = mapWallet.at(txin.prevout.hash);
        coin.MarkDirty();
        NotifyTransactionChanged(coin.GetHash(), CT_UPDATED);
    }

    // Get the inserted-CWalletTx from mapWallet so that the
    // fInMempool flag is cached properly
    CWalletTx& wtx = mapWallet.at(tx->GetHash());

    if (!fBroadcastTransactions) {
        // Don't submit tx to the mempool
        return;
    }

    std::string err_string;
    if (!wtx.SubmitMemoryPoolAndRelay(err_string, true)) {
        WalletLogPrintf("CommitTransaction(): Transaction cannot be broadcast immediately, %s\n", err_string);
        // TODO: if we expect the failure to be long term or permanent, instead delete wtx from the wallet and return failure.
    }
}

uint64_t CWallet::GetStakeWeight(uint64_t* pStakerWeight, uint64_t* pDelegateWeight) const
{
    uint64_t nWeight = 0;
    uint64_t nStakerWeight = 0;
    uint64_t nDelegateWeight = 0;
    if(pStakerWeight) *pStakerWeight = nStakerWeight;
    if(pDelegateWeight) *pDelegateWeight = nDelegateWeight;

    // Choose coins to use
    const auto bal = GetBalance();
    CAmount nBalance = bal.m_mine_trusted;
    if(IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS))
        nBalance += bal.m_watchonly_trusted;

    if (nBalance <= m_reserve_balance)
        return nWeight;

    std::set<std::pair<const CWalletTx*,unsigned int> > setCoins;
    CAmount nValueIn = 0;

    CAmount nTargetValue = nBalance - m_reserve_balance;
    if (!SelectCoinsForStaking(nTargetValue, setCoins, nValueIn))
        return nWeight;

    if (setCoins.empty())
        return nWeight;

    int nHeight = GetLastBlockHeight() + 1;
    int coinbaseMaturity = Params().GetConsensus().CoinbaseMaturity(nHeight);
    bool canSuperStake = false;
    for(std::pair<const CWalletTx*,unsigned int> pcoin : setCoins)
    {
        if (pcoin.first->GetDepthInMainChain() >= coinbaseMaturity)
        {
            // Compute staker weight
            CAmount nValue = pcoin.first->tx->vout[pcoin.second].nValue;
            nStakerWeight += nValue;

            // Check if the staker can super stake
            if(!canSuperStake && nValue >= DEFAULT_STAKING_MIN_UTXO_VALUE)
                canSuperStake = true;
        }
    }

    if(canSuperStake)
    {
        // Get the weight of the delegated coins
        std::vector<COutPoint> vDelegateCoins;
        std::map<uint160, CAmount> mDelegateWeight;
        SelectDelegateCoinsForStaking(vDelegateCoins, mDelegateWeight);
        for(const COutPoint &prevout : vDelegateCoins)
        {
            Coin coinPrev;
            if(!chain().getUnspentOutput(prevout, coinPrev)){
                continue;
            }

            nDelegateWeight += coinPrev.out.nValue;
        }
    }

    nWeight = nStakerWeight + nDelegateWeight;
    if(pStakerWeight) *pStakerWeight = nStakerWeight;
    if(pDelegateWeight) *pDelegateWeight = nDelegateWeight;

    return nWeight;
}

bool CWallet::CreateCoinStakeFromMine(unsigned int nBits, const CAmount& nTotalFees, uint32_t nTimeBlock, CMutableTransaction& tx, PKHash& pkhash, std::set<std::pair<const CWalletTx*,unsigned int> >& setCoins, std::vector<COutPoint>& setSelectedCoins, bool selectedOnly, bool sign, COutPoint& headerPrevout)
{
    bool fAllowWatchOnly = IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    CBlockIndex* pindexPrev = chain().getTip();
    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    struct CMutableTransaction txNew(tx);
    txNew.vin.clear();
    txNew.vout.clear();

    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));

    // Choose coins to use
    const auto bal = GetBalance();
    CAmount nBalance = bal.m_mine_trusted;
    if(fAllowWatchOnly)
        nBalance += bal.m_watchonly_trusted;

    if (nBalance <= m_reserve_balance)
        return false;

    std::vector<std::pair<const CWalletTx*,unsigned int>> vwtxPrev;

    if (setCoins.empty())
        return false;

    if(stakeCache.size() > setCoins.size() + 100){
        //Determining if the cache is still valid is harder than just clearing it when it gets too big, so instead just clear it
        //when it has more than 100 entries more than the actual setCoins.
        stakeCache.clear();
    }
    if(!fHasMinerStakeCache && gArgs.GetBoolArg("-stakecache", DEFAULT_STAKE_CACHE)) {

        for(const std::pair<const CWalletTx*,unsigned int> &pcoin : setCoins)
        {
            boost::this_thread::interruption_point();
            COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);
            CacheKernel(stakeCache, prevoutStake, pindexPrev, chain().getCoinsTip()); //this will do a 2 disk loads per op
        }
    }
    std::map<COutPoint, CStakeCache>& cache = fHasMinerStakeCache ? minerStakeCache : stakeCache;
    int64_t nCredit = 0;
    CScript scriptPubKeyKernel;
    CScript aggregateScriptPubKeyHashKernel;

    // Populate the list with the selected coins
    std::set<std::pair<const CWalletTx*,unsigned int> > setSelected;
    if(selectedOnly)
    {
        for(const COutPoint& prevoutStake : setSelectedCoins)
        {
            auto it = mapWallet.find(prevoutStake.hash);
            if (it != mapWallet.end()) {
                setSelected.insert(std::make_pair(&it->second, prevoutStake.n));
            }
        }
    }

    std::set<std::pair<const CWalletTx*,unsigned int> >& setPrevouts = selectedOnly ? setSelected : setCoins;
    for(const std::pair<const CWalletTx*,unsigned int> &pcoin : setPrevouts)
    {
        bool fKernelFound = false;
        boost::this_thread::interruption_point();
        // Search backward in time from the given txNew timestamp
        // Search nSearchInterval seconds back up to nMaxStakeSearchInterval
        COutPoint prevoutStake = COutPoint(pcoin.first->GetHash(), pcoin.second);
        if (CheckKernel(pindexPrev, nBits, nTimeBlock, prevoutStake, chain().getCoinsTip(), cache, chain().chainman().ActiveChain()))
        {
            // Found a kernel
            LogPrint(BCLog::COINSTAKE, "CreateCoinStake : kernel found\n");
            std::vector<valtype> vSolutions;
            CScript scriptPubKeyOut;
            scriptPubKeyKernel = pcoin.first->tx->vout[pcoin.second].scriptPubKey;
            TxoutType whichType = Solver(scriptPubKeyKernel, vSolutions);
            if (whichType == TxoutType::NONSTANDARD)
            {
                LogPrint(BCLog::COINSTAKE, "CreateCoinStake : failed to parse kernel\n");
                break;
            }
            LogPrint(BCLog::COINSTAKE, "CreateCoinStake : parsed kernel type=%d\n", (int)whichType);
            if (whichType != TxoutType::PUBKEY && whichType != TxoutType::PUBKEYHASH)
            {
                LogPrint(BCLog::COINSTAKE, "CreateCoinStake : no support for kernel type=%d\n", (int)whichType);
                break;  // only support pay to public key and pay to address
            }
            if (whichType == TxoutType::PUBKEYHASH) // pay to address type
            {
                // convert to pay to public key type
                uint160 hash160(vSolutions[0]);
                pkhash = PKHash(hash160);
                CPubKey pubKeyStake;
                if (!HasPrivateKey(pkhash, fAllowWatchOnly) || !GetPubKey(pkhash, pubKeyStake))
                {
                    LogPrint(BCLog::COINSTAKE, "CreateCoinStake : failed to get key for kernel type=%d\n", (int)whichType);
                    break;  // unable to find corresponding public key
                }
                scriptPubKeyOut << pubKeyStake.getvch() << OP_CHECKSIG;
                aggregateScriptPubKeyHashKernel = scriptPubKeyKernel;
            }
            if (whichType == TxoutType::PUBKEY)
            {
                valtype& vchPubKey = vSolutions[0];
                CPubKey pubKey(vchPubKey);
                uint160 hash160(Hash160(vchPubKey));
                pkhash = PKHash(hash160);
                CPubKey pubKeyStake;
                if (!HasPrivateKey(pkhash, fAllowWatchOnly) || !GetPubKey(pkhash, pubKeyStake))
                {
                    LogPrint(BCLog::COINSTAKE, "CreateCoinStake : failed to get key for kernel type=%d\n", (int)whichType);
                    break;  // unable to find corresponding public key
                }

                if (pubKeyStake != pubKey)
                {
                    LogPrint(BCLog::COINSTAKE, "CreateCoinStake : invalid key for kernel type=%d\n", (int)whichType);
                    break; // keys mismatch
                }

                scriptPubKeyOut = scriptPubKeyKernel;
                aggregateScriptPubKeyHashKernel = CScript() << OP_DUP << OP_HASH160 << ToByteVector(hash160) << OP_EQUALVERIFY << OP_CHECKSIG;
            }

            txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
            nCredit += pcoin.first->tx->vout[pcoin.second].nValue;
            vwtxPrev.push_back(pcoin);
            txNew.vout.push_back(CTxOut(0, scriptPubKeyOut));

            LogPrint(BCLog::COINSTAKE, "CreateCoinStake : added kernel type=%d\n", (int)whichType);
            fKernelFound = true;
        }

        if (fKernelFound)
        {
            headerPrevout = prevoutStake;
            break; // if kernel is found stop searching
        }
    }

    if (nCredit == 0 || nCredit > nBalance - m_reserve_balance)
        return false;

    for(const std::pair<const CWalletTx*,unsigned int> &pcoin : setCoins)
    {
        // Attempt to add more inputs
        // Only add coins of the same key/address as kernel
        if (txNew.vout.size() == 2 && ((pcoin.first->tx->vout[pcoin.second].scriptPubKey == scriptPubKeyKernel || pcoin.first->tx->vout[pcoin.second].scriptPubKey == aggregateScriptPubKeyHashKernel))
                && pcoin.first->GetHash() != txNew.vin[0].prevout.hash)
        {
            // Stop adding more inputs if already too many inputs
            if (txNew.vin.size() >= GetStakeMaxCombineInputs())
                break;
            // Stop adding inputs if reached reserve limit
            if (nCredit + pcoin.first->tx->vout[pcoin.second].nValue > nBalance - m_reserve_balance)
                break;
            // Do not add additional significant input
            if (pcoin.first->tx->vout[pcoin.second].nValue >= GetStakeCombineThreshold())
                continue;

            txNew.vin.push_back(CTxIn(pcoin.first->GetHash(), pcoin.second));
            nCredit += pcoin.first->tx->vout[pcoin.second].nValue;
            vwtxPrev.push_back(pcoin);
        }
    }

    const Consensus::Params& consensusParams = Params().GetConsensus();
    int64_t nRewardPiece = 0;
    // Calculate reward
    {
        int64_t nReward = nTotalFees + GetBlockSubsidy(pindexPrev->nHeight + 1, consensusParams);
        if (nReward < 0)
            return false;

        if(pindexPrev->nHeight < consensusParams.nFirstMPoSBlock || pindexPrev->nHeight >= consensusParams.nLastMPoSBlock)
        {
            // Keep whole reward
            nCredit += nReward;
        }
        else
        {
            // Split the reward when mpos is used
            nRewardPiece = nReward / consensusParams.nMPoSRewardRecipients;
            nCredit += nRewardPiece + nReward % consensusParams.nMPoSRewardRecipients;
        }
   }

    if (nCredit >= GetStakeSplitThreshold())
    {
        for(unsigned int i = 0; i < GetStakeSplitOutputs() - 1; i++)
            txNew.vout.push_back(CTxOut(0, txNew.vout[1].scriptPubKey)); //split stake
    }

    // Set output amount
    if (txNew.vout.size() == GetStakeSplitOutputs() + 1)
    {
        CAmount nValue = (nCredit / GetStakeSplitOutputs() / CENT) * CENT;
        for(unsigned int i = 1; i < GetStakeSplitOutputs(); i++)
            txNew.vout[i].nValue = nValue;
        txNew.vout[GetStakeSplitOutputs()].nValue = nCredit - nValue * (GetStakeSplitOutputs() - 1);
    }
    else
        txNew.vout[1].nValue = nCredit;

    if(pindexPrev->nHeight >= consensusParams.nFirstMPoSBlock && pindexPrev->nHeight < consensusParams.nLastMPoSBlock)
    {
        if(!CreateMPoSOutputs(txNew, nRewardPiece, pindexPrev->nHeight, consensusParams, chain().chainman().ActiveChain()))
            return error("CreateCoinStake : failed to create MPoS reward outputs");
    }

    // Append the Refunds To Sender to the transaction outputs
    for(unsigned int i = 2; i < tx.vout.size(); i++)
    {
        txNew.vout.push_back(tx.vout[i]);
    }

    // Sign the input coins
    if(sign && !SignTransactionStake(txNew, vwtxPrev))
        return error("CreateCoinStake : failed to sign coinstake");

    // Successfully generated coinstake
    tx = txNew;
    return true;
}

bool CWallet::CreateCoinStakeFromDelegate(unsigned int nBits, const CAmount& nTotalFees, uint32_t nTimeBlock, CMutableTransaction& tx, PKHash& pkhash, std::set<std::pair<const CWalletTx*,unsigned int> >& setCoins, std::vector<COutPoint>& setDelegateCoins, bool sign, std::vector<unsigned char>& vchPoD, COutPoint& headerPrevout)
{
    bool fAllowWatchOnly = IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    CBlockIndex* pindexPrev = chain().getTip();
    arith_uint256 bnTargetPerCoinDay;
    bnTargetPerCoinDay.SetCompact(nBits);

    struct CMutableTransaction txNew(tx);
    txNew.vin.clear();
    txNew.vout.clear();

    // Mark coin stake transaction
    CScript scriptEmpty;
    scriptEmpty.clear();
    txNew.vout.push_back(CTxOut(0, scriptEmpty));

    std::vector<std::pair<const CWalletTx*,unsigned int>> vwtxPrev;
    if (setDelegateCoins.empty())
        return false;

    if(stakeDelegateCache.size() > setDelegateCoins.size() + 100){
        //Determining if the cache is still valid is harder than just clearing it when it gets too big, so instead just clear it
        //when it has more than 100 entries more than the actual setCoins.
        stakeDelegateCache.clear();
    }
    if(!fHasMinerStakeCache && gArgs.GetBoolArg("-stakecache", DEFAULT_STAKE_CACHE)) {

        for(const COutPoint &prevoutStake : setDelegateCoins)
        {
            boost::this_thread::interruption_point();
            CacheKernel(stakeDelegateCache, prevoutStake, pindexPrev, chain().getCoinsTip()); //this will do a 2 disk loads per op
        }
    }
    std::map<COutPoint, CStakeCache>& cache = fHasMinerStakeCache ? minerStakeCache : stakeDelegateCache;
    int64_t nCredit = 0;
    CScript scriptPubKeyKernel;
    CScript scriptPubKeyStaker;
    Delegation delegation;
    bool delegateOutputExist = false;

    for(const COutPoint &prevoutStake : setDelegateCoins)
    {
        bool fKernelFound = false;
        boost::this_thread::interruption_point();
        // Search backward in time from the given txNew timestamp
        // Search nSearchInterval seconds back up to nMaxStakeSearchInterval
        if (CheckKernel(pindexPrev, nBits, nTimeBlock, prevoutStake, chain().getCoinsTip(), cache, chain().chainman().ActiveChain()))
        {
            // Found a kernel
            LogPrint(BCLog::COINSTAKE, "CreateCoinStake : kernel found\n");
            std::vector<valtype> vSolutions;

            Coin coinPrev;
            if(!chain().getUnspentOutput(prevoutStake, coinPrev)){
                if(!GetSpentCoinFromMainChain(pindexPrev, prevoutStake, &coinPrev, chain().chainman().ActiveChain())) {
                    return error("CreateCoinStake: Could not find coin and it was not at the tip");
                }
            }

            scriptPubKeyKernel = coinPrev.out.scriptPubKey;
            TxoutType whichType = Solver(scriptPubKeyKernel, vSolutions);
            if (whichType == TxoutType::NONSTANDARD)
            {
                LogPrint(BCLog::COINSTAKE, "CreateCoinStake : failed to parse kernel\n");
                break;
            }
            LogPrint(BCLog::COINSTAKE, "CreateCoinStake : parsed kernel type=%d\n", (int)whichType);
            if (whichType != TxoutType::PUBKEY && whichType != TxoutType::PUBKEYHASH)
            {
                LogPrint(BCLog::COINSTAKE, "CreateCoinStake : no support for kernel type=%d\n", (int)whichType);
                break;  // only support pay to public key and pay to address
            }
            if (whichType == TxoutType::PUBKEYHASH) // pay to address type
            {
                // convert to pay to public key type
                uint160 hash160(vSolutions[0]);

                if(!GetDelegationStaker(hash160, delegation))
                    return error("CreateCoinStake: Failed to find delegation");

                pkhash = PKHash(delegation.staker);
                CPubKey pubKeyStake;
                if (!HasPrivateKey(pkhash, fAllowWatchOnly) || !GetPubKey(pkhash, pubKeyStake))
                {
                    LogPrint(BCLog::COINSTAKE, "CreateCoinStake : failed to get staker key for kernel type=%d\n", (int)whichType);
                    break;  // unable to find corresponding public key
                }
                scriptPubKeyStaker << pubKeyStake.getvch() << OP_CHECKSIG;
            }
            if (whichType == TxoutType::PUBKEY)
            {
                valtype& vchPubKey = vSolutions[0];
                uint160 hash160(Hash160(vchPubKey));;

                if(!GetDelegationStaker(hash160, delegation))
                    return error("CreateCoinStake: Failed to find delegation");

                pkhash = PKHash(delegation.staker);
                CPubKey pubKeyStake;
                if (!HasPrivateKey(pkhash, fAllowWatchOnly) || !GetPubKey(pkhash, pubKeyStake))
                {
                    LogPrint(BCLog::COINSTAKE, "CreateCoinStake : failed to get staker key for kernel type=%d\n", (int)whichType);
                    break;  // unable to find corresponding public key
                }

                scriptPubKeyStaker << pubKeyStake.getvch() << OP_CHECKSIG;
            }

            delegateOutputExist = IsDelegateOutputExist(delegation.fee);
            PKHash superStakerAddress(delegation.staker);
            COutPoint prevoutSuperStaker;
            CAmount nValueSuperStaker = 0;
            const CWalletTx* pcoinSuperStaker = GetCoinSuperStaker(setCoins, superStakerAddress, prevoutSuperStaker, nValueSuperStaker);
            if(!pcoinSuperStaker)
            {
                LogPrint(BCLog::COINSTAKE, "CreateCoinStake : failed to get utxo for super staker %s\n", EncodeDestination(superStakerAddress));
                break;  // unable to find utxo from the super staker
            }

            txNew.vin.push_back(CTxIn(prevoutSuperStaker));
            nCredit += nValueSuperStaker;
            vwtxPrev.push_back(std::make_pair(pcoinSuperStaker, prevoutSuperStaker.n));
            txNew.vout.push_back(CTxOut(0, scriptPubKeyStaker));
            if(delegateOutputExist)
            {
                txNew.vout.push_back(CTxOut(0, scriptPubKeyKernel));
            }

            LogPrint(BCLog::COINSTAKE, "CreateCoinStake : added kernel type=%d\n", (int)whichType);
            fKernelFound = true;
        }

        if (fKernelFound)
        {
            headerPrevout = prevoutStake;
            break; // if kernel is found stop searching
        }
    }

    if (nCredit == 0)
        return false;

    const Consensus::Params& consensusParams = Params().GetConsensus();
    int64_t nRewardPiece = 0;
    int64_t nRewardOffline = 0;
    // Calculate reward
    {
        int64_t nTotalReward = nTotalFees + GetBlockSubsidy(pindexPrev->nHeight + 1, consensusParams);
        if (nTotalReward < 0)
            return false;

        if(pindexPrev->nHeight < consensusParams.nFirstMPoSBlock || pindexPrev->nHeight >= consensusParams.nLastMPoSBlock)
        {
            // Keep whole reward
            int64_t nRewardStaker = 0;
            if(!SplitOfflineStakeReward(nTotalReward, delegation.fee, nRewardOffline, nRewardStaker))
                return error("CreateCoinStake: Failed to split reward");
            nCredit += nRewardStaker;
        }
        else
        {
            // Split the reward when mpos is used
            nRewardPiece = nTotalReward / consensusParams.nMPoSRewardRecipients;
            int64_t nRewardStaker = 0;
            int64_t nReward = nRewardPiece + nTotalReward % consensusParams.nMPoSRewardRecipients;
            if(!SplitOfflineStakeReward(nReward, delegation.fee, nRewardOffline, nRewardStaker))
                return error("CreateCoinStake: Failed to split reward");
            nCredit += nRewardStaker;
        }
    }

    // Set output amount
    txNew.vout[1].nValue = nCredit;
    if(delegateOutputExist)
    {
        txNew.vout[2].nValue = nRewardOffline;
    }

    if(pindexPrev->nHeight >= consensusParams.nFirstMPoSBlock && pindexPrev->nHeight < consensusParams.nLastMPoSBlock)
    {
        if(!CreateMPoSOutputs(txNew, nRewardPiece, pindexPrev->nHeight, consensusParams, chain().chainman().ActiveChain()))
            return error("CreateCoinStake : failed to create MPoS reward outputs");
    }

    // Append the Refunds To Sender to the transaction outputs
    for(unsigned int i = 2; i < tx.vout.size(); i++)
    {
        txNew.vout.push_back(tx.vout[i]);
    }

    // Sign the input coins
    if(sign && !SignTransactionStake(txNew, vwtxPrev))
        return error("CreateCoinStake : failed to sign coinstake");

    // Successfully generated coinstake
    tx = txNew;

    // Save Proof Of Delegation
    vchPoD = delegation.PoD;

    return true;
}

bool CWallet::GetDelegationStaker(const uint160& keyid, Delegation& delegation)
{
    std::map<uint160, Delegation>::iterator it = m_delegations_staker.find(keyid);
    if(it == m_delegations_staker.end())
        return false;

    delegation = it->second;
    return true;
}

const CWalletTx* CWallet::GetCoinSuperStaker(const std::set<std::pair<const CWalletTx*,unsigned int> >& setCoins, const PKHash& superStaker, COutPoint& prevout, CAmount& nValueRet)
{
    for(const std::pair<const CWalletTx*,unsigned int> &pcoin : setCoins)
    {
        CAmount nValue = pcoin.first->tx->vout[pcoin.second].nValue;
        if(nValue < DEFAULT_STAKING_MIN_UTXO_VALUE)
            continue;

        CScript scriptPubKey = pcoin.first->tx->vout[pcoin.second].scriptPubKey;
        bool OK = false;
        PKHash pkhash = ExtractPublicKeyHash(scriptPubKey, &OK);
        if(OK && pkhash == superStaker)
        {
            nValueRet = nValue;
            prevout = COutPoint(pcoin.first->GetHash(), pcoin.second);
            return pcoin.first;
        }
    }

    return 0;
}

bool CWallet::CanSuperStake(const std::set<std::pair<const CWalletTx*,unsigned int> >& setCoins, const std::vector<COutPoint>& setDelegateCoins) const
{
    bool canSuperStake = false;
    if(setDelegateCoins.size() > 0)
    {
        for(const std::pair<const CWalletTx*,unsigned int> &pcoin : setCoins)
        {
            CAmount nValue = pcoin.first->tx->vout[pcoin.second].nValue;
            if(nValue >= DEFAULT_STAKING_MIN_UTXO_VALUE)
            {
                canSuperStake = true;
                break;
            }
        }
    }

    return canSuperStake;
}

bool CWallet::CreateCoinStake(unsigned int nBits, const CAmount& nTotalFees, uint32_t nTimeBlock, CMutableTransaction& tx, PKHash& pkhash, std::set<std::pair<const CWalletTx*,unsigned int> >& setCoins, std::vector<COutPoint>& setSelectedCoins, std::vector<COutPoint>& setDelegateCoins, bool selectedOnly, bool sign, std::vector<unsigned char>& vchPoD, COutPoint& headerPrevout)
{
    // Can super stake
    bool canSuperStake = CanSuperStake(setCoins, setDelegateCoins);

    // Create coinstake from coins that are delegated to me
    if(canSuperStake && CreateCoinStakeFromDelegate(nBits, nTotalFees, nTimeBlock, tx, pkhash, setCoins, setDelegateCoins, sign, vchPoD, headerPrevout))
        return true;

    // Create coinstake from coins that are mine
    if(setCoins.size() > 0 && CreateCoinStakeFromMine(nBits, nTotalFees, nTimeBlock, tx, pkhash, setCoins, setSelectedCoins, selectedOnly, sign, headerPrevout))
        return true;

    // Fail to create coinstake
    return false;
}

DBErrors CWallet::LoadWallet()
{
    LOCK(cs_wallet);

    DBErrors nLoadWalletRet = WalletBatch(GetDatabase()).LoadWallet(this);
    if (nLoadWalletRet == DBErrors::NEED_REWRITE)
    {
        if (GetDatabase().Rewrite("\x04pool"))
        {
            for (const auto& spk_man_pair : m_spk_managers) {
                spk_man_pair.second->RewriteDB();
            }
        }
    }

    if (m_spk_managers.empty()) {
        assert(m_external_spk_managers.empty());
        assert(m_internal_spk_managers.empty());
    }

    if (nLoadWalletRet != DBErrors::LOAD_OK)
        return nLoadWalletRet;

    return DBErrors::LOAD_OK;
}

DBErrors CWallet::ZapSelectTx(std::vector<uint256>& vHashIn, std::vector<uint256>& vHashOut)
{
    AssertLockHeld(cs_wallet);
    DBErrors nZapSelectTxRet = WalletBatch(GetDatabase()).ZapSelectTx(vHashIn, vHashOut);
    for (const uint256& hash : vHashOut) {
        const auto& it = mapWallet.find(hash);
        wtxOrdered.erase(it->second.m_it_wtxOrdered);
        for (const auto& txin : it->second.tx->vin)
            mapTxSpends.erase(txin.prevout);
        mapWallet.erase(it);
        NotifyTransactionChanged(hash, CT_DELETED);
    }

    if (nZapSelectTxRet == DBErrors::NEED_REWRITE)
    {
        if (GetDatabase().Rewrite("\x04pool"))
        {
            for (const auto& spk_man_pair : m_spk_managers) {
                spk_man_pair.second->RewriteDB();
            }
        }
    }

    if (nZapSelectTxRet != DBErrors::LOAD_OK)
        return nZapSelectTxRet;

    MarkDirty();

    return DBErrors::LOAD_OK;
}

bool CWallet::SetAddressBookWithDB(WalletBatch& batch, const CTxDestination& address, const std::string& strName, const std::string& strPurpose)
{
    bool fUpdated = false;
    bool is_mine;
    {
        LOCK(cs_wallet);
        std::map<CTxDestination, CAddressBookData>::iterator mi = m_address_book.find(address);
        fUpdated = (mi != m_address_book.end() && !mi->second.IsChange());
        m_address_book[address].SetLabel(strName);
        if (!strPurpose.empty()) /* update purpose only if requested */
            m_address_book[address].purpose = strPurpose;
        is_mine = IsMine(address) != ISMINE_NO;
    }
    NotifyAddressBookChanged(address, strName, is_mine,
                             strPurpose, (fUpdated ? CT_UPDATED : CT_NEW));
    if (!strPurpose.empty() && !batch.WritePurpose(EncodeDestination(address), strPurpose))
        return false;
    return batch.WriteName(EncodeDestination(address), strName);
}

bool CWallet::SetAddressBook(const CTxDestination& address, const std::string& strName, const std::string& strPurpose)
{
    WalletBatch batch(GetDatabase());
    return SetAddressBookWithDB(batch, address, strName, strPurpose);
}

bool CWallet::DelAddressBook(const CTxDestination& address)
{
    bool is_mine;
    WalletBatch batch(GetDatabase());
    {
        LOCK(cs_wallet);
        // If we want to delete receiving addresses, we need to take care that DestData "used" (and possibly newer DestData) gets preserved (and the "deleted" address transformed into a change entry instead of actually being deleted)
        // NOTE: This isn't a problem for sending addresses because they never have any DestData yet!
        // When adding new DestData, it should be considered here whether to retain or delete it (or move it?).
        if (IsMine(address)) {
            WalletLogPrintf("%s called with IsMine address, NOT SUPPORTED. Please report this bug! %s\n", __func__, PACKAGE_BUGREPORT);
            return false;
        }
        // Delete destdata tuples associated with address
        std::string strAddress = EncodeDestination(address);
        for (const std::pair<const std::string, std::string> &item : m_address_book[address].destdata)
        {
            batch.EraseDestData(strAddress, item.first);
        }
        m_address_book.erase(address);
        is_mine = IsMine(address) != ISMINE_NO;
    }

    NotifyAddressBookChanged(address, "", is_mine, "", CT_DELETED);

    batch.ErasePurpose(EncodeDestination(address));
    return batch.EraseName(EncodeDestination(address));
}

size_t CWallet::KeypoolCountExternalKeys() const
{
    AssertLockHeld(cs_wallet);

    auto legacy_spk_man = GetLegacyScriptPubKeyMan();
    if (legacy_spk_man) {
        return legacy_spk_man->KeypoolCountExternalKeys();
    }

    unsigned int count = 0;
    for (auto spk_man : m_external_spk_managers) {
        count += spk_man.second->GetKeyPoolSize();
    }

    return count;
}

unsigned int CWallet::GetKeyPoolSize() const
{
    AssertLockHeld(cs_wallet);

    unsigned int count = 0;
    for (auto spk_man : GetActiveScriptPubKeyMans()) {
        count += spk_man->GetKeyPoolSize();
    }
    return count;
}

bool CWallet::TopUpKeyPool(unsigned int kpSize)
{
    LOCK(cs_wallet);
    bool res = true;
    for (auto spk_man : GetActiveScriptPubKeyMans()) {
        res &= spk_man->TopUp(kpSize);
    }
    return res;
}

bool CWallet::GetNewDestination(const OutputType type, const std::string label, CTxDestination& dest, std::string& error)
{
    LOCK(cs_wallet);
    error.clear();
    bool result = false;
    auto spk_man = GetScriptPubKeyMan(type, false /* internal */);
    if (spk_man) {
        spk_man->TopUp();
        result = spk_man->GetNewDestination(type, dest, error);
    } else {
        error = strprintf(_("Error: No %s addresses available."), FormatOutputType(type)).translated;
    }
    if (result) {
        SetAddressBook(dest, label, "receive");
    }

    return result;
}

bool CWallet::GetNewChangeDestination(const OutputType type, CTxDestination& dest, std::string& error)
{
    LOCK(cs_wallet);
    error.clear();

    ReserveDestination reservedest(this, type);
    if (!reservedest.GetReservedDestination(dest, true, error)) {
        return false;
    }

    reservedest.KeepDestination();
    return true;
}

int64_t CWallet::GetOldestKeyPoolTime() const
{
    LOCK(cs_wallet);
    int64_t oldestKey = std::numeric_limits<int64_t>::max();
    for (const auto& spk_man_pair : m_spk_managers) {
        oldestKey = std::min(oldestKey, spk_man_pair.second->GetOldestKeyPoolTime());
    }
    return oldestKey;
}

void CWallet::MarkDestinationsDirty(const std::set<CTxDestination>& destinations) {
    for (auto& entry : mapWallet) {
        CWalletTx& wtx = entry.second;
        if (wtx.m_is_cache_empty) continue;
        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            CTxDestination dst;
            if (ExtractDestination(wtx.tx->vout[i].scriptPubKey, dst) && destinations.count(dst)) {
                wtx.MarkDirty();
                break;
            }
        }
    }
}

std::set<CTxDestination> CWallet::GetLabelAddresses(const std::string& label) const
{
    LOCK(cs_wallet);
    std::set<CTxDestination> result;
    for (const std::pair<const CTxDestination, CAddressBookData>& item : m_address_book)
    {
        if (item.second.IsChange()) continue;
        const CTxDestination& address = item.first;
        const std::string& strName = item.second.GetLabel();
        if (strName == label)
            result.insert(address);
    }
    return result;
}

// disable transaction (only for coinstake)
void CWallet::DisableTransaction(const CTransaction &tx)
{
    if (!tx.IsCoinStake() || !IsFromMe(tx))
        return; // only disconnecting coinstake requires marking input unspent

    uint256 hash = tx.GetHash();
    if(AbandonTransaction(hash))
    {
        LOCK(cs_wallet);
        RemoveFromSpends(hash);
        for(const CTxIn& txin : tx.vin)
        {
            auto it = mapWallet.find(txin.prevout.hash);
            if (it != mapWallet.end()) {
                CWalletTx &coin = it->second;
                coin.MarkDirty();
                NotifyTransactionChanged(coin.GetHash(), CT_UPDATED);
            }
        }
        CWalletTx& wtx = mapWallet.at(hash);
        wtx.MarkDirty();
        NotifyTransactionChanged(hash, CT_DELETED);
    }
}

bool ReserveDestination::GetReservedDestination(CTxDestination& dest, bool internal, std::string& error)
{
    m_spk_man = pwallet->GetScriptPubKeyMan(type, internal);
    if (!m_spk_man) {
        error = strprintf(_("Error: No %s addresses available."), FormatOutputType(type)).translated;
        return false;
    }


    if (nIndex == -1)
    {
        m_spk_man->TopUp();

        CKeyPool keypool;
        if (!m_spk_man->GetReservedDestination(type, internal, address, nIndex, keypool, error)) {
            return false;
        }
        fInternal = keypool.fInternal;
    }
    dest = address;
    return true;
}

void ReserveDestination::KeepDestination()
{
    if (nIndex != -1) {
        m_spk_man->KeepDestination(nIndex, type);
    }
    nIndex = -1;
    address = CNoDestination();
}

void ReserveDestination::ReturnDestination()
{
    if (nIndex != -1) {
        m_spk_man->ReturnDestination(nIndex, fInternal, address);
    }
    nIndex = -1;
    address = CNoDestination();
}

bool CWallet::DisplayAddress(const CTxDestination& dest)
{
    CScript scriptPubKey = GetScriptForDestination(dest);
    const auto spk_man = GetScriptPubKeyMan(scriptPubKey);
    if (spk_man == nullptr) {
        return false;
    }
    auto signer_spk_man = dynamic_cast<ExternalSignerScriptPubKeyMan*>(spk_man);
    if (signer_spk_man == nullptr) {
        return false;
    }
    ExternalSigner signer = ExternalSignerScriptPubKeyMan::GetExternalSigner();
    return signer_spk_man->DisplayAddress(scriptPubKey, signer);
}

void CWallet::LockCoin(const COutPoint& output)
{
    AssertLockHeld(cs_wallet);
    setLockedCoins.insert(output);
}

void CWallet::UnlockCoin(const COutPoint& output)
{
    AssertLockHeld(cs_wallet);
    setLockedCoins.erase(output);
}

void CWallet::UnlockAllCoins()
{
    AssertLockHeld(cs_wallet);
    setLockedCoins.clear();
}

bool CWallet::IsLockedCoin(uint256 hash, unsigned int n) const
{
#ifndef DEBUG_LOCKORDER
    AssertLockHeld(cs_wallet);
#endif
    COutPoint outpt(hash, n);

    return (setLockedCoins.count(outpt) > 0);
}

void CWallet::ListLockedCoins(std::vector<COutPoint>& vOutpts) const
{
    AssertLockHeld(cs_wallet);
    for (std::set<COutPoint>::iterator it = setLockedCoins.begin();
         it != setLockedCoins.end(); it++) {
        COutPoint outpt = (*it);
        vOutpts.push_back(outpt);
    }
}

/** @} */ // end of Actions

void CWallet::GetKeyBirthTimes(std::map<CKeyID, int64_t>& mapKeyBirth) const {
    AssertLockHeld(cs_wallet);
    mapKeyBirth.clear();

    // map in which we'll infer heights of other keys
    std::map<CKeyID, const CWalletTx::Confirmation*> mapKeyFirstBlock;
    CWalletTx::Confirmation max_confirm;
    max_confirm.block_height = GetLastBlockHeight() > 144 ? GetLastBlockHeight() - 144 : 0; // the tip can be reorganized; use a 144-block safety margin
    CHECK_NONFATAL(chain().findAncestorByHeight(GetLastBlockHash(), max_confirm.block_height, FoundBlock().hash(max_confirm.hashBlock)));

    {
        LegacyScriptPubKeyMan* spk_man = GetLegacyScriptPubKeyMan();
        assert(spk_man != nullptr);
        LOCK(spk_man->cs_KeyStore);

        // get birth times for keys with metadata
        for (const auto& entry : spk_man->mapKeyMetadata) {
            if (entry.second.nCreateTime) {
                mapKeyBirth[entry.first] = entry.second.nCreateTime;
            }
        }

        // Prepare to infer birth heights for keys without metadata
        for (const CKeyID &keyid : spk_man->GetKeys()) {
            if (mapKeyBirth.count(keyid) == 0)
                mapKeyFirstBlock[keyid] = &max_confirm;
        }

        // if there are no such keys, we're done
        if (mapKeyFirstBlock.empty())
            return;

        // find first block that affects those keys, if there are any left
        for (const auto& entry : mapWallet) {
            // iterate over all wallet transactions...
            const CWalletTx &wtx = entry.second;
            if (wtx.m_confirm.status == CWalletTx::CONFIRMED) {
                // ... which are already in a block
                for (const CTxOut &txout : wtx.tx->vout) {
                    // iterate over all their outputs
                    for (const auto &keyid : GetAffectedKeys(txout.scriptPubKey, *spk_man)) {
                        // ... and all their affected keys
                        auto rit = mapKeyFirstBlock.find(keyid);
                        if (rit != mapKeyFirstBlock.end() && wtx.m_confirm.block_height < rit->second->block_height) {
                            rit->second = &wtx.m_confirm;
                        }
                    }
                }
            }
        }
    }

    // Extract block timestamps for those keys
    for (const auto& entry : mapKeyFirstBlock) {
        int64_t block_time;
        CHECK_NONFATAL(chain().findBlock(entry.second->hashBlock, FoundBlock().time(block_time)));
        mapKeyBirth[entry.first] = block_time - TIMESTAMP_WINDOW; // block times can be 2h off
    }
}

/**
 * Compute smart timestamp for a transaction being added to the wallet.
 *
 * Logic:
 * - If sending a transaction, assign its timestamp to the current time.
 * - If receiving a transaction outside a block, assign its timestamp to the
 *   current time.
 * - If receiving a block with a future timestamp, assign all its (not already
 *   known) transactions' timestamps to the current time.
 * - If receiving a block with a past timestamp, before the most recent known
 *   transaction (that we care about), assign all its (not already known)
 *   transactions' timestamps to the same timestamp as that most-recent-known
 *   transaction.
 * - If receiving a block with a past timestamp, but after the most recent known
 *   transaction, assign all its (not already known) transactions' timestamps to
 *   the block time.
 *
 * For more information see CWalletTx::nTimeSmart,
 * https://bitcointalk.org/?topic=54527, or
 * https://github.com/bitcoin/bitcoin/pull/1393.
 */
unsigned int CWallet::ComputeTimeSmart(const CWalletTx& wtx) const
{
    unsigned int nTimeSmart = wtx.nTimeReceived;
    if (!wtx.isUnconfirmed() && !wtx.isAbandoned()) {
        int64_t blocktime;
        if (chain().findBlock(wtx.m_confirm.hashBlock, FoundBlock().time(blocktime))) {
            int64_t latestNow = wtx.nTimeReceived;
            int64_t latestEntry = 0;

            // Tolerate times up to the last timestamp in the wallet not more than 5 minutes into the future
            int64_t latestTolerated = latestNow + 300;
            const TxItems& txOrdered = wtxOrdered;
            for (auto it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
                CWalletTx* const pwtx = it->second;
                if (pwtx == &wtx) {
                    continue;
                }
                int64_t nSmartTime;
                nSmartTime = pwtx->nTimeSmart;
                if (!nSmartTime) {
                    nSmartTime = pwtx->nTimeReceived;
                }
                if (nSmartTime <= latestTolerated) {
                    latestEntry = nSmartTime;
                    if (nSmartTime > latestNow) {
                        latestNow = nSmartTime;
                    }
                    break;
                }
            }

            nTimeSmart = std::max(latestEntry, std::min(blocktime, latestNow));
        } else {
            WalletLogPrintf("%s: found %s in block %s not in index\n", __func__, wtx.GetHash().ToString(), wtx.m_confirm.hashBlock.ToString());
        }
    }
    return nTimeSmart;
}

bool CWallet::SetAddressUsed(WalletBatch& batch, const CTxDestination& dest, bool used)
{
    const std::string key{"used"};
    if (std::get_if<CNoDestination>(&dest))
        return false;

    if (!used) {
        if (auto* data = util::FindKey(m_address_book, dest)) data->destdata.erase(key);
        return batch.EraseDestData(EncodeDestination(dest), key);
    }

    const std::string value{"1"};
    m_address_book[dest].destdata.insert(std::make_pair(key, value));
    return batch.WriteDestData(EncodeDestination(dest), key, value);
}

void CWallet::LoadDestData(const CTxDestination &dest, const std::string &key, const std::string &value)
{
    m_address_book[dest].destdata.insert(std::make_pair(key, value));
}

bool CWallet::IsAddressUsed(const CTxDestination& dest) const
{
    const std::string key{"used"};
    std::map<CTxDestination, CAddressBookData>::const_iterator i = m_address_book.find(dest);
    if(i != m_address_book.end())
    {
        CAddressBookData::StringMap::const_iterator j = i->second.destdata.find(key);
        if(j != i->second.destdata.end())
        {
            return true;
        }
    }
    return false;
}

std::vector<std::string> CWallet::GetAddressReceiveRequests() const
{
    const std::string prefix{"rr"};
    std::vector<std::string> values;
    for (const auto& address : m_address_book) {
        for (const auto& data : address.second.destdata) {
            if (!data.first.compare(0, prefix.size(), prefix)) {
                values.emplace_back(data.second);
            }
        }
    }
    return values;
}

bool CWallet::SetAddressReceiveRequest(WalletBatch& batch, const CTxDestination& dest, const std::string& id, const std::string& value)
{
    const std::string key{"rr" + id}; // "rr" prefix = "receive request" in destdata
    CAddressBookData& data = m_address_book.at(dest);
    if (value.empty()) {
        if (!batch.EraseDestData(EncodeDestination(dest), key)) return false;
        data.destdata.erase(key);
    } else {
        if (!batch.WriteDestData(EncodeDestination(dest), key, value)) return false;
        data.destdata[key] = value;
    }
    return true;
}

std::unique_ptr<WalletDatabase> MakeWalletDatabase(const std::string& name, const DatabaseOptions& options, DatabaseStatus& status, bilingual_str& error_string)
{
    // Do some checking on wallet path. It should be either a:
    //
    // 1. Path where a directory can be created.
    // 2. Path to an existing directory.
    // 3. Path to a symlink to a directory.
    // 4. For backwards compatibility, the name of a data file in -walletdir.
    const fs::path wallet_path = fsbridge::AbsPathJoin(GetWalletDir(), name);
    fs::file_type path_type = fs::symlink_status(wallet_path).type();
    if (!(path_type == fs::file_not_found || path_type == fs::directory_file ||
          (path_type == fs::symlink_file && fs::is_directory(wallet_path)) ||
          (path_type == fs::regular_file && fs::path(name).filename() == name))) {
        error_string = Untranslated(strprintf(
              "Invalid -wallet path '%s'. -wallet path should point to a directory where wallet.dat and "
              "database/log.?????????? files can be stored, a location where such a directory could be created, "
              "or (for backwards compatibility) the name of an existing data file in -walletdir (%s)",
              name, GetWalletDir()));
        status = DatabaseStatus::FAILED_BAD_PATH;
        return nullptr;
    }
    return MakeDatabase(wallet_path, options, status, error_string);
}

std::shared_ptr<CWallet> CWallet::Create(interfaces::Chain* chain, const std::string& name, std::unique_ptr<WalletDatabase> database, uint64_t wallet_creation_flags, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    const std::string& walletFile = database->Filename();

    int64_t nStart = GetTimeMillis();
    // TODO: Can't use std::make_shared because we need a custom deleter but
    // should be possible to use std::allocate_shared.
    std::shared_ptr<CWallet> walletInstance(new CWallet(chain, name, std::move(database)), ReleaseWallet);
    DBErrors nLoadWalletRet = walletInstance->LoadWallet();
    if (nLoadWalletRet != DBErrors::LOAD_OK) {
        if (nLoadWalletRet == DBErrors::CORRUPT) {
            error = strprintf(_("Error loading %s: Wallet corrupted"), walletFile);
            return nullptr;
        }
        else if (nLoadWalletRet == DBErrors::NONCRITICAL_ERROR)
        {
            warnings.push_back(strprintf(_("Error reading %s! All keys read correctly, but transaction data"
                                           " or address book entries might be missing or incorrect."),
                walletFile));
        }
        else if (nLoadWalletRet == DBErrors::TOO_NEW) {
            error = strprintf(_("Error loading %s: Wallet requires newer version of %s"), walletFile, PACKAGE_NAME);
            return nullptr;
        }
        else if (nLoadWalletRet == DBErrors::NEED_REWRITE)
        {
            error = strprintf(_("Wallet needed to be rewritten: restart %s to complete"), PACKAGE_NAME);
            return nullptr;
        }
        else {
            error = strprintf(_("Error loading %s"), walletFile);
            return nullptr;
        }
    }

    // This wallet is in its first run if there are no ScriptPubKeyMans and it isn't blank or no privkeys
    const bool fFirstRun = walletInstance->m_spk_managers.empty() &&
                     !walletInstance->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS) &&
                     !walletInstance->IsWalletFlagSet(WALLET_FLAG_BLANK_WALLET);
    if (fFirstRun)
    {
        // ensure this wallet.dat can only be opened by clients supporting HD with chain split and expects no default key
        walletInstance->SetMinVersion(FEATURE_LATEST);

        walletInstance->AddWalletFlags(wallet_creation_flags);

        // Only create LegacyScriptPubKeyMan when not descriptor wallet
        if (!walletInstance->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
            walletInstance->SetupLegacyScriptPubKeyMan();
        }

        if ((wallet_creation_flags & WALLET_FLAG_EXTERNAL_SIGNER) || !(wallet_creation_flags & (WALLET_FLAG_DISABLE_PRIVATE_KEYS | WALLET_FLAG_BLANK_WALLET))) {
            LOCK(walletInstance->cs_wallet);
            if (walletInstance->IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
                walletInstance->SetupDescriptorScriptPubKeyMans();
                // SetupDescriptorScriptPubKeyMans already calls SetupGeneration for us so we don't need to call SetupGeneration separately
            } else {
                // Legacy wallets need SetupGeneration here.
                for (auto spk_man : walletInstance->GetActiveScriptPubKeyMans()) {
                    if (!spk_man->SetupGeneration()) {
                        error = _("Unable to generate initial keys");
                        return nullptr;
                    }
                }
            }
        }

        if (chain) {
            walletInstance->chainStateFlushed(chain->getTipLocator());
        }
    } else if (wallet_creation_flags & WALLET_FLAG_DISABLE_PRIVATE_KEYS) {
        // Make it impossible to disable private keys after creation
        error = strprintf(_("Error loading %s: Private keys can only be disabled during creation"), walletFile);
        return NULL;
    } else if (walletInstance->IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        for (auto spk_man : walletInstance->GetActiveScriptPubKeyMans()) {
            if (spk_man->HavePrivateKeys()) {
                warnings.push_back(strprintf(_("Warning: Private keys detected in wallet {%s} with disabled private keys"), walletFile));
                break;
            }
        }
    }

    if (!gArgs.GetArg("-addresstype", "").empty()) {
        if (!ParseOutputType(gArgs.GetArg("-addresstype", ""), walletInstance->m_default_address_type)) {
            error = strprintf(_("Unknown address type '%s'"), gArgs.GetArg("-addresstype", ""));
            return nullptr;
        }
    }

    if (!gArgs.GetArg("-changetype", "").empty()) {
        OutputType out_type;
        if (!ParseOutputType(gArgs.GetArg("-changetype", ""), out_type)) {
            error = strprintf(_("Unknown change type '%s'"), gArgs.GetArg("-changetype", ""));
            return nullptr;
        }
        walletInstance->m_default_change_type = out_type;
    }

    if (gArgs.IsArgSet("-mintxfee")) {
        CAmount n = 0;
        if (!ParseMoney(gArgs.GetArg("-mintxfee", ""), n) || 0 == n) {
            error = AmountErrMsg("mintxfee", gArgs.GetArg("-mintxfee", ""));
            return nullptr;
        }
        if (n > HIGH_TX_FEE_PER_KB) {
            warnings.push_back(AmountHighWarn("-mintxfee") + Untranslated(" ") +
                               _("This is the minimum transaction fee you pay on every transaction."));
        }
        walletInstance->m_min_fee = CFeeRate(n);
    }

    if (gArgs.IsArgSet("-maxapsfee")) {
        const std::string max_aps_fee{gArgs.GetArg("-maxapsfee", "")};
        CAmount n = 0;
        if (max_aps_fee == "-1") {
            n = -1;
        } else if (!ParseMoney(max_aps_fee, n)) {
            error = AmountErrMsg("maxapsfee", max_aps_fee);
            return nullptr;
        }
        if (n > HIGH_APS_FEE) {
            warnings.push_back(AmountHighWarn("-maxapsfee") + Untranslated(" ") +
                              _("This is the maximum transaction fee you pay (in addition to the normal fee) to prioritize partial spend avoidance over regular coin selection."));
        }
        walletInstance->m_max_aps_fee = n;
    }

    if (gArgs.IsArgSet("-fallbackfee")) {
        CAmount nFeePerK = 0;
        if (!ParseMoney(gArgs.GetArg("-fallbackfee", ""), nFeePerK)) {
            error = strprintf(_("Invalid amount for -fallbackfee=<amount>: '%s'"), gArgs.GetArg("-fallbackfee", ""));
            return nullptr;
        }
        if (nFeePerK > HIGH_TX_FEE_PER_KB) {
            warnings.push_back(AmountHighWarn("-fallbackfee") + Untranslated(" ") +
                               _("This is the transaction fee you may pay when fee estimates are not available."));
        }
        walletInstance->m_fallback_fee = CFeeRate(nFeePerK);
    }
    // Disable fallback fee in case value was set to 0, enable if non-null value
    walletInstance->m_allow_fallback_fee = walletInstance->m_fallback_fee.GetFeePerK() != 0;

    if (gArgs.IsArgSet("-discardfee")) {
        CAmount nFeePerK = 0;
        if (!ParseMoney(gArgs.GetArg("-discardfee", ""), nFeePerK)) {
            error = strprintf(_("Invalid amount for -discardfee=<amount>: '%s'"), gArgs.GetArg("-discardfee", ""));
            return nullptr;
        }
        if (nFeePerK > HIGH_TX_FEE_PER_KB) {
            warnings.push_back(AmountHighWarn("-discardfee") + Untranslated(" ") +
                               _("This is the transaction fee you may discard if change is smaller than dust at this level"));
        }
        walletInstance->m_discard_rate = CFeeRate(nFeePerK);
    }
    if (gArgs.IsArgSet("-paytxfee")) {
        CAmount nFeePerK = 0;
        if (!ParseMoney(gArgs.GetArg("-paytxfee", ""), nFeePerK)) {
            error = AmountErrMsg("paytxfee", gArgs.GetArg("-paytxfee", ""));
            return nullptr;
        }
        if (nFeePerK > HIGH_TX_FEE_PER_KB) {
            warnings.push_back(AmountHighWarn("-paytxfee") + Untranslated(" ") +
                               _("This is the transaction fee you will pay if you send a transaction."));
        }
        walletInstance->m_pay_tx_fee = CFeeRate(nFeePerK, 1000);
        if (chain && walletInstance->m_pay_tx_fee < chain->relayMinFee()) {
            error = strprintf(_("Invalid amount for -paytxfee=<amount>: '%s' (must be at least %s)"),
                gArgs.GetArg("-paytxfee", ""), chain->relayMinFee().ToString());
            return nullptr;
        }
    }

    if (gArgs.IsArgSet("-maxtxfee")) {
        CAmount nMaxFee = 0;
        if (!ParseMoney(gArgs.GetArg("-maxtxfee", ""), nMaxFee)) {
            error = AmountErrMsg("maxtxfee", gArgs.GetArg("-maxtxfee", ""));
            return nullptr;
        }
        if (nMaxFee > HIGH_MAX_TX_FEE) {
            warnings.push_back(_("-maxtxfee is set very high! Fees this large could be paid on a single transaction."));
        }
        if (chain && CFeeRate(nMaxFee, 1000) < chain->relayMinFee()) {
            error = strprintf(_("Invalid amount for -maxtxfee=<amount>: '%s' (must be at least the minrelay fee of %s to prevent stuck transactions)"),
                gArgs.GetArg("-maxtxfee", ""), chain->relayMinFee().ToString());
            return nullptr;
        }
        walletInstance->m_default_max_tx_fee = nMaxFee;
    }

    if (chain && chain->relayMinFee().GetFeePerK() > HIGH_TX_FEE_PER_KB) {
        warnings.push_back(AmountHighWarn("-minrelaytxfee") + Untranslated(" ") +
                           _("The wallet will avoid paying less than the minimum relay fee."));
    }

    walletInstance->m_confirm_target = gArgs.GetArg("-txconfirmtarget", DEFAULT_TX_CONFIRM_TARGET);
    walletInstance->m_spend_zero_conf_change = gArgs.GetBoolArg("-spendzeroconfchange", DEFAULT_SPEND_ZEROCONF_CHANGE);
    walletInstance->m_signal_rbf = gArgs.GetBoolArg("-walletrbf", DEFAULT_WALLET_RBF);
    if(!ParseMoney(gArgs.GetArg("-reservebalance", FormatMoney(DEFAULT_RESERVE_BALANCE)), walletInstance->m_reserve_balance))
        walletInstance->m_reserve_balance = DEFAULT_RESERVE_BALANCE;
    walletInstance->m_use_change_address = gArgs.GetBoolArg("-usechangeaddress", DEFAULT_USE_CHANGE_ADDRESS);
    if(!ParseMoney(gArgs.GetArg("-stakingminutxovalue", FormatMoney(DEFAULT_STAKING_MIN_UTXO_VALUE)), walletInstance->m_staking_min_utxo_value))
        walletInstance->m_staking_min_utxo_value = DEFAULT_STAKING_MIN_UTXO_VALUE;
    if(!ParseMoney(gArgs.GetArg("-minstakerutxosize", FormatMoney(DEFAULT_STAKER_MIN_UTXO_SIZE)), walletInstance->m_staker_min_utxo_size))
        walletInstance->m_staker_min_utxo_size = DEFAULT_STAKER_MIN_UTXO_SIZE;
    if (gArgs.IsArgSet("-stakingminfee"))
    {
        int nStakingMinFee = gArgs.GetArg("-stakingminfee", DEFAULT_STAKING_MIN_FEE);
        if(nStakingMinFee < 0 || nStakingMinFee > 100)
        {
            chain->initError(strprintf(_("Invalid percentage value for -stakingminfee=<n>: '%d' (must be between 0 and 100)"), nStakingMinFee));
            return nullptr;
        }
        walletInstance->m_staking_min_fee = nStakingMinFee;
    }
    walletInstance->m_staker_max_utxo_script_cache = gArgs.GetArg("-maxstakerutxoscriptcache", DEFAULT_STAKER_MAX_UTXO_SCRIPT_CACHE);
    walletInstance->m_num_threads = gArgs.GetArg("-stakerthreads", GetNumCores());
    walletInstance->m_num_threads = std::max(1, walletInstance->m_num_threads);
    walletInstance->m_ledger_id = gArgs.GetArg("-stakerledgerid", "");

    walletInstance->WalletLogPrintf("Wallet completed loading in %15dms\n", GetTimeMillis() - nStart);

    // Try to top up keypool. No-op if the wallet is locked.
    walletInstance->TopUpKeyPool();

    LOCK(walletInstance->cs_wallet);

    if (chain && !AttachChain(walletInstance, *chain, error, warnings)) {
        return nullptr;
    }

    {
        LOCK(cs_wallets);
        for (auto& load_wallet : g_load_wallet_fns) {
            load_wallet(interfaces::MakeWallet(walletInstance));
        }
    }

    walletInstance->SetBroadcastTransactions(gArgs.GetBoolArg("-walletbroadcast", DEFAULT_WALLETBROADCAST));

    {
        walletInstance->WalletLogPrintf("setKeyPool.size() = %u\n",      walletInstance->GetKeyPoolSize());
        walletInstance->WalletLogPrintf("mapWallet.size() = %u\n",       walletInstance->mapWallet.size());
        walletInstance->WalletLogPrintf("m_address_book.size() = %u\n",  walletInstance->m_address_book.size());
    }

    if(!fReindex)
        // Clean not reverted coinstake transactions
        walletInstance->CleanCoinStake();

    return walletInstance;
}

bool CWallet::AttachChain(const std::shared_ptr<CWallet>& walletInstance, interfaces::Chain& chain, bilingual_str& error, std::vector<bilingual_str>& warnings)
{
    LOCK(walletInstance->cs_wallet);
    // allow setting the chain if it hasn't been set already but prevent changing it
    assert(!walletInstance->m_chain || walletInstance->m_chain == &chain);
    walletInstance->m_chain = &chain;

    // Register wallet with validationinterface. It's done before rescan to avoid
    // missing block connections between end of rescan and validation subscribing.
    // Because of wallet lock being hold, block connection notifications are going to
    // be pending on the validation-side until lock release. It's likely to have
    // block processing duplicata (if rescan block range overlaps with notification one)
    // but we guarantee at least than wallet state is correct after notifications delivery.
    // This is temporary until rescan and notifications delivery are unified under same
    // interface.
    walletInstance->m_chain_notifications_handler = walletInstance->chain().handleNotifications(walletInstance);

    int rescan_height = 0;
    if (!gArgs.GetBoolArg("-rescan", false))
    {
        WalletBatch batch(walletInstance->GetDatabase());
        CBlockLocator locator;
        if (batch.ReadBestBlock(locator)) {
            if (const std::optional<int> fork_height = chain.findLocatorFork(locator)) {
                rescan_height = *fork_height;
            }
        }
    }

    const std::optional<int> tip_height = chain.getHeight();
    if (tip_height) {
        walletInstance->m_last_block_processed = chain.getBlockHash(*tip_height);
        walletInstance->m_last_block_processed_height = *tip_height;
    } else {
        walletInstance->m_last_block_processed.SetNull();
        walletInstance->m_last_block_processed_height = -1;
    }

    if (tip_height && *tip_height != rescan_height)
    {
        if (chain.havePruned()) {
            int block_height = *tip_height;
            while (block_height > 0 && chain.haveBlockOnDisk(block_height - 1) && rescan_height != block_height) {
                --block_height;
            }

            if (rescan_height != block_height) {
                // We can't rescan beyond non-pruned blocks, stop and throw an error.
                // This might happen if a user uses an old wallet within a pruned node
                // or if they ran -disablewallet for a longer time, then decided to re-enable
                // Exit early and print an error.
                // If a block is pruned after this check, we will load the wallet,
                // but fail the rescan with a generic error.
                error = _("Prune: last wallet synchronisation goes beyond pruned data. You need to -reindex (download the whole blockchain again in case of pruned node)");
                return false;
            }
        }

        chain.initMessage(_("Rescanning…").translated);
        walletInstance->WalletLogPrintf("Rescanning last %i blocks (from block %i)...\n", *tip_height - rescan_height, rescan_height);

        // No need to read and scan block if block was created before
        // our wallet birthday (as adjusted for block time variability)
        std::optional<int64_t> time_first_key;
        for (auto spk_man : walletInstance->GetAllScriptPubKeyMans()) {
            int64_t time = spk_man->GetTimeFirstKey();
            if (!time_first_key || time < *time_first_key) time_first_key = time;
        }
        if (time_first_key) {
            chain.findFirstBlockWithTimeAndHeight(*time_first_key - TIMESTAMP_WINDOW, rescan_height, FoundBlock().height(rescan_height));
        }

        {
            WalletRescanReserver reserver(*walletInstance);
            if (!reserver.reserve() || (ScanResult::SUCCESS != walletInstance->ScanForWalletTransactions(chain.getBlockHash(rescan_height), rescan_height, {} /* max height */, reserver, true /* update */).status)) {
                error = _("Failed to rescan the wallet during initialization");
                return false;
            }
        }
        walletInstance->chainStateFlushed(chain.getTipLocator());
        walletInstance->GetDatabase().IncrementUpdateCounter();
    }

    return true;
}

const CAddressBookData* CWallet::FindAddressBookEntry(const CTxDestination& dest, bool allow_change) const
{
    const auto& address_book_it = m_address_book.find(dest);
    if (address_book_it == m_address_book.end()) return nullptr;
    if ((!allow_change) && address_book_it->second.IsChange()) {
        return nullptr;
    }
    return &address_book_it->second;
}

bool CWallet::UpgradeWallet(int version, bilingual_str& error)
{
    int prev_version = GetVersion();
    if (version == 0) {
        WalletLogPrintf("Performing wallet upgrade to %i\n", FEATURE_LATEST);
        version = FEATURE_LATEST;
    } else {
        WalletLogPrintf("Allowing wallet upgrade up to %i\n", version);
    }
    if (version < prev_version) {
        error = strprintf(_("Cannot downgrade wallet from version %i to version %i. Wallet version unchanged."), prev_version, version);
        return false;
    }

    LOCK(cs_wallet);

    // Do not upgrade versions to any version between HD_SPLIT and FEATURE_PRE_SPLIT_KEYPOOL unless already supporting HD_SPLIT
    if (!CanSupportFeature(FEATURE_HD_SPLIT) && version >= FEATURE_HD_SPLIT && version < FEATURE_PRE_SPLIT_KEYPOOL) {
        error = strprintf(_("Cannot upgrade a non HD split wallet from version %i to version %i without upgrading to support pre-split keypool. Please use version %i or no version specified."), prev_version, version, FEATURE_PRE_SPLIT_KEYPOOL);
        return false;
    }

    // Permanently upgrade to the version
    SetMinVersion(GetClosestWalletFeature(version));

    for (auto spk_man : GetActiveScriptPubKeyMans()) {
        if (!spk_man->Upgrade(prev_version, version, error)) {
            return false;
        }
    }
    return true;
}

void CWallet::postInitProcess()
{
    LOCK(cs_wallet);

    // Add wallet transactions that aren't already in a block to mempool
    // Do this here as mempool requires genesis block to be loaded
    ReacceptWalletTransactions();

    // Update wallet transactions with current mempool transactions.
    chain().requestMempoolTransactions(*this);

    // Start mine proof-of-stake blocks in the background
    if (CanStake()) {
        StartStake();
    }
}

bool CWallet::BackupWallet(const std::string& strDest) const
{
    return GetDatabase().Backup(strDest);
}

bool CWallet::LoadToken(const CTokenInfo &token)
{
    uint256 hash = token.GetHash();
    mapToken[hash] = token;

    return true;
}

bool CWallet::LoadTokenTx(const CTokenTx &tokenTx)
{
    uint256 hash = tokenTx.GetHash();
    mapTokenTx[hash] = tokenTx;

    return true;
}

bool CWallet::AddTokenEntry(const CTokenInfo &token, bool fFlushOnClose)
{
    LOCK(cs_wallet);

    WalletBatch batch(GetDatabase(), fFlushOnClose);

    uint256 hash = token.GetHash();

    bool fInsertedNew = true;

    std::map<uint256, CTokenInfo>::iterator it = mapToken.find(hash);
    if(it!=mapToken.end())
    {
        fInsertedNew = false;
    }

    // Write to disk
    CTokenInfo wtoken = token;
    if(!fInsertedNew)
    {
        wtoken.nCreateTime = chain().getAdjustedTime();
    }
    else
    {
        wtoken.nCreateTime = it->second.nCreateTime;
    }

    if (!batch.WriteToken(wtoken))
        return false;

    mapToken[hash] = wtoken;

    NotifyTokenChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

    // Refresh token tx
    if(fInsertedNew)
    {
        for(auto it = mapTokenTx.begin(); it != mapTokenTx.end(); it++)
        {
            uint256 tokenTxHash = it->second.GetHash();
            NotifyTokenTransactionChanged(this, tokenTxHash, CT_UPDATED);
        }
    }

    LogPrintf("AddTokenEntry %s\n", wtoken.GetHash().ToString());

    return true;
}

bool CWallet::AddTokenTxEntry(const CTokenTx &tokenTx, bool fFlushOnClose)
{
    LOCK(cs_wallet);

    WalletBatch batch(GetDatabase(), fFlushOnClose);

    uint256 hash = tokenTx.GetHash();

    bool fInsertedNew = true;

    std::map<uint256, CTokenTx>::iterator it = mapTokenTx.find(hash);
    if(it!=mapTokenTx.end())
    {
        fInsertedNew = false;
    }

    // Write to disk
    CTokenTx wtokenTx = tokenTx;
    if(!fInsertedNew)
    {
        wtokenTx.strLabel = it->second.strLabel;
    }
    int64_t blockTime;
    uint256 blockHash = wtokenTx.blockNumber < 0 ? uint256() : chain().getBlockHash(wtokenTx.blockNumber);
    bool found = !blockHash.IsNull() && chain().findBlock(blockHash, FoundBlock().time(blockTime));
    wtokenTx.nCreateTime = found ? blockTime : chain().getAdjustedTime();

    if (!batch.WriteTokenTx(wtokenTx))
        return false;

    mapTokenTx[hash] = wtokenTx;

    NotifyTokenTransactionChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

    LogPrintf("AddTokenTxEntry %s\n", wtokenTx.GetHash().ToString());

    return true;
}

CKeyPool::CKeyPool()
{
    nTime = GetTime();
    fInternal = false;
    m_pre_split = false;
}

CKeyPool::CKeyPool(const CPubKey& vchPubKeyIn, bool internalIn)
{
    nTime = GetTime();
    vchPubKey = vchPubKeyIn;
    fInternal = internalIn;
    m_pre_split = false;
}

int CWalletTx::GetDepthInMainChain() const
{
    assert(pwallet != nullptr);
#ifndef DEBUG_LOCKORDER
    AssertLockHeld(pwallet->cs_wallet);
#endif
    if (isUnconfirmed() || isAbandoned()) return 0;

    return (pwallet->GetLastBlockHeight() - m_confirm.block_height + 1) * (isConflicted() ? -1 : 1);
}

int CWalletTx::GetBlocksToMaturity() const
{
    if (!(IsCoinBase() || IsCoinStake()))
        return 0;
    int chain_depth = GetDepthInMainChain();
    int nHeight = pwallet->GetLastBlockHeight() + 1;
    int coinbaseMaturity = Params().GetConsensus().CoinbaseMaturity(nHeight);
    return std::max(0, (coinbaseMaturity+1) - chain_depth);
}

bool CWalletTx::IsImmature() const
{
    // note GetBlocksToMaturity is 0 for non-coinbase tx
    return GetBlocksToMaturity() > 0;
}

bool CWalletTx::IsImmatureCoinBase() const
{
    return IsCoinBase() && IsImmature();
}

bool CWalletTx::IsImmatureCoinStake() const
{
    return IsCoinStake() && IsImmature();
}

bool CWallet::IsCrypted() const
{
    return HasEncryptionKeys();
}

bool CWallet::IsLocked() const
{
    if (!IsCrypted()) {
        return false;
    }
    LOCK(cs_wallet);
    return vMasterKey.empty();
}

bool CWallet::Lock()
{
    if (!IsCrypted())
        return false;

    {
        LOCK(cs_wallet);
        vMasterKey.clear();
    }

    NotifyStatusChanged(this);
    return true;
}

bool CWallet::Unlock(const CKeyingMaterial& vMasterKeyIn, bool accept_no_keys)
{
    {
        LOCK(cs_wallet);
        for (const auto& spk_man_pair : m_spk_managers) {
            if (!spk_man_pair.second->CheckDecryptionKey(vMasterKeyIn, accept_no_keys)) {
                return false;
            }
        }
        vMasterKey = vMasterKeyIn;
    }
    NotifyStatusChanged(this);
    return true;
}

std::set<ScriptPubKeyMan*> CWallet::GetActiveScriptPubKeyMans() const
{
    std::set<ScriptPubKeyMan*> spk_mans;
    for (bool internal : {false, true}) {
        for (OutputType t : OUTPUT_TYPES) {
            auto spk_man = GetScriptPubKeyMan(t, internal);
            if (spk_man) {
                spk_mans.insert(spk_man);
            }
        }
    }
    return spk_mans;
}

std::set<ScriptPubKeyMan*> CWallet::GetAllScriptPubKeyMans() const
{
    std::set<ScriptPubKeyMan*> spk_mans;
    for (const auto& spk_man_pair : m_spk_managers) {
        spk_mans.insert(spk_man_pair.second.get());
    }
    return spk_mans;
}

ScriptPubKeyMan* CWallet::GetScriptPubKeyMan(const OutputType& type, bool internal) const
{
    const std::map<OutputType, ScriptPubKeyMan*>& spk_managers = internal ? m_internal_spk_managers : m_external_spk_managers;
    std::map<OutputType, ScriptPubKeyMan*>::const_iterator it = spk_managers.find(type);
    if (it == spk_managers.end()) {
        return nullptr;
    }
    return it->second;
}

std::set<ScriptPubKeyMan*> CWallet::GetScriptPubKeyMans(const CScript& script, SignatureData& sigdata) const
{
    std::set<ScriptPubKeyMan*> spk_mans;
    for (const auto& spk_man_pair : m_spk_managers) {
        if (spk_man_pair.second->CanProvide(script, sigdata)) {
            spk_mans.insert(spk_man_pair.second.get());
        }
    }
    return spk_mans;
}

ScriptPubKeyMan* CWallet::GetScriptPubKeyMan(const CScript& script) const
{
    SignatureData sigdata;
    for (const auto& spk_man_pair : m_spk_managers) {
        if (spk_man_pair.second->CanProvide(script, sigdata)) {
            return spk_man_pair.second.get();
        }
    }
    return nullptr;
}

ScriptPubKeyMan* CWallet::GetScriptPubKeyMan(const uint256& id) const
{
    if (m_spk_managers.count(id) > 0) {
        return m_spk_managers.at(id).get();
    }
    return nullptr;
}

std::unique_ptr<SigningProvider> CWallet::GetSolvingProvider(const CScript& script) const
{
    SignatureData sigdata;
    return GetSolvingProvider(script, sigdata);
}

std::unique_ptr<SigningProvider> CWallet::GetSolvingProvider(const CScript& script, SignatureData& sigdata) const
{
    for (const auto& spk_man_pair : m_spk_managers) {
        if (spk_man_pair.second->CanProvide(script, sigdata)) {
            return spk_man_pair.second->GetSolvingProvider(script);
        }
    }
    return nullptr;
}

LegacyScriptPubKeyMan* CWallet::GetLegacyScriptPubKeyMan() const
{
    if (IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        return nullptr;
    }
    // Legacy wallets only have one ScriptPubKeyMan which is a LegacyScriptPubKeyMan.
    // Everything in m_internal_spk_managers and m_external_spk_managers point to the same legacyScriptPubKeyMan.
    auto it = m_internal_spk_managers.find(OutputType::LEGACY);
    if (it == m_internal_spk_managers.end()) return nullptr;
    return dynamic_cast<LegacyScriptPubKeyMan*>(it->second);
}

LegacyScriptPubKeyMan* CWallet::GetOrCreateLegacyScriptPubKeyMan()
{
    SetupLegacyScriptPubKeyMan();
    return GetLegacyScriptPubKeyMan();
}

void CWallet::SetupLegacyScriptPubKeyMan()
{
    if (!m_internal_spk_managers.empty() || !m_external_spk_managers.empty() || !m_spk_managers.empty() || IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        return;
    }

    auto spk_manager = std::unique_ptr<ScriptPubKeyMan>(new LegacyScriptPubKeyMan(*this));
    for (const auto& type : LEGACY_OUTPUT_TYPES) {
        m_internal_spk_managers[type] = spk_manager.get();
        m_external_spk_managers[type] = spk_manager.get();
    }
    m_spk_managers[spk_manager->GetID()] = std::move(spk_manager);
}

const CKeyingMaterial& CWallet::GetEncryptionKey() const
{
    return vMasterKey;
}

bool CWallet::HasEncryptionKeys() const
{
    return !mapMasterKeys.empty();
}

void CWallet::ConnectScriptPubKeyManNotifiers()
{
    for (const auto& spk_man : GetActiveScriptPubKeyMans()) {
        spk_man->NotifyWatchonlyChanged.connect(NotifyWatchonlyChanged);
        spk_man->NotifyCanGetAddressesChanged.connect(NotifyCanGetAddressesChanged);
    }
}

void CWallet::LoadDescriptorScriptPubKeyMan(uint256 id, WalletDescriptor& desc)
{
    if (IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER)) {
        auto spk_manager = std::unique_ptr<ScriptPubKeyMan>(new ExternalSignerScriptPubKeyMan(*this, desc));
        m_spk_managers[id] = std::move(spk_manager);
    } else {
        auto spk_manager = std::unique_ptr<ScriptPubKeyMan>(new DescriptorScriptPubKeyMan(*this, desc));
        m_spk_managers[id] = std::move(spk_manager);
    }
}

void CWallet::SetupDescriptorScriptPubKeyMans()
{
    AssertLockHeld(cs_wallet);

    if (!IsWalletFlagSet(WALLET_FLAG_EXTERNAL_SIGNER)) {
        // Make a seed
        CKey seed_key;
        seed_key.MakeNewKey(true);
        CPubKey seed = seed_key.GetPubKey();
        assert(seed_key.VerifyPubKey(seed));

        // Get the extended key
        CExtKey master_key;
        master_key.SetSeed(seed_key.begin(), seed_key.size());

        for (bool internal : {false, true}) {
            for (OutputType t : OUTPUT_TYPES) {
                if (t == OutputType::BECH32M) {
                    // Skip taproot (bech32m) for now
                    // TODO: Setup taproot (bech32m) descriptors by default
                    continue;
                }
                auto spk_manager = std::unique_ptr<DescriptorScriptPubKeyMan>(new DescriptorScriptPubKeyMan(*this));
                if (IsCrypted()) {
                    if (IsLocked()) {
                        throw std::runtime_error(std::string(__func__) + ": Wallet is locked, cannot setup new descriptors");
                    }
                    if (!spk_manager->CheckDecryptionKey(vMasterKey) && !spk_manager->Encrypt(vMasterKey, nullptr)) {
                        throw std::runtime_error(std::string(__func__) + ": Could not encrypt new descriptors");
                    }
                }
                spk_manager->SetupDescriptorGeneration(master_key, t, internal);
                uint256 id = spk_manager->GetID();
                m_spk_managers[id] = std::move(spk_manager);
                AddActiveScriptPubKeyMan(id, t, internal);
            }
        }
    } else {
        ExternalSigner signer = ExternalSignerScriptPubKeyMan::GetExternalSigner();

        // TODO: add account parameter
        int account = 0;
        UniValue signer_res = signer.GetDescriptors(account);

        if (!signer_res.isObject()) throw std::runtime_error(std::string(__func__) + ": Unexpected result");
        for (bool internal : {false, true}) {
            const UniValue& descriptor_vals = find_value(signer_res, internal ? "internal" : "receive");
            if (!descriptor_vals.isArray()) throw std::runtime_error(std::string(__func__) + ": Unexpected result");
            for (const UniValue& desc_val : descriptor_vals.get_array().getValues()) {
                std::string desc_str = desc_val.getValStr();
                FlatSigningProvider keys;
                std::string dummy_error;
                std::unique_ptr<Descriptor> desc = Parse(desc_str, keys, dummy_error, false);
                if (!desc->GetOutputType()) {
                    continue;
                }
                OutputType t =  *desc->GetOutputType();
                auto spk_manager = std::unique_ptr<ExternalSignerScriptPubKeyMan>(new ExternalSignerScriptPubKeyMan(*this));
                spk_manager->SetupDescriptor(std::move(desc));
                uint256 id = spk_manager->GetID();
                m_spk_managers[id] = std::move(spk_manager);
                AddActiveScriptPubKeyMan(id, t, internal);
            }
        }
    }
}

void CWallet::AddActiveScriptPubKeyMan(uint256 id, OutputType type, bool internal)
{
    WalletBatch batch(GetDatabase());
    if (!batch.WriteActiveScriptPubKeyMan(static_cast<uint8_t>(type), id, internal)) {
        throw std::runtime_error(std::string(__func__) + ": writing active ScriptPubKeyMan id failed");
    }
    LoadActiveScriptPubKeyMan(id, type, internal);
}

void CWallet::LoadActiveScriptPubKeyMan(uint256 id, OutputType type, bool internal)
{
    // Activating ScriptPubKeyManager for a given output and change type is incompatible with legacy wallets.
    // Legacy wallets have only one ScriptPubKeyManager and it's active for all output and change types.
    Assert(IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS));

    WalletLogPrintf("Setting spkMan to active: id = %s, type = %d, internal = %d\n", id.ToString(), static_cast<int>(type), static_cast<int>(internal));
    auto& spk_mans = internal ? m_internal_spk_managers : m_external_spk_managers;
    auto& spk_mans_other = internal ? m_external_spk_managers : m_internal_spk_managers;
    auto spk_man = m_spk_managers.at(id).get();
    spk_mans[type] = spk_man;

    if (spk_mans_other[type] == spk_man) {
        spk_mans_other.erase(type);
    }

    NotifyCanGetAddressesChanged();
}

void CWallet::DeactivateScriptPubKeyMan(uint256 id, OutputType type, bool internal)
{
    auto spk_man = GetScriptPubKeyMan(type, internal);
    if (spk_man != nullptr && spk_man->GetID() == id) {
        WalletLogPrintf("Deactivate spkMan: id = %s, type = %d, internal = %d\n", id.ToString(), static_cast<int>(type), static_cast<int>(internal));
        WalletBatch batch(GetDatabase());
        if (!batch.EraseActiveScriptPubKeyMan(static_cast<uint8_t>(type), internal)) {
            throw std::runtime_error(std::string(__func__) + ": erasing active ScriptPubKeyMan id failed");
        }

        auto& spk_mans = internal ? m_internal_spk_managers : m_external_spk_managers;
        spk_mans.erase(type);
    }

    NotifyCanGetAddressesChanged();
}

bool CWallet::IsLegacy() const
{
    if (m_internal_spk_managers.count(OutputType::LEGACY) == 0) {
        return false;
    }
    auto spk_man = dynamic_cast<LegacyScriptPubKeyMan*>(m_internal_spk_managers.at(OutputType::LEGACY));
    return spk_man != nullptr;
}

DescriptorScriptPubKeyMan* CWallet::GetDescriptorScriptPubKeyMan(const WalletDescriptor& desc) const
{
    for (auto& spk_man_pair : m_spk_managers) {
        // Try to downcast to DescriptorScriptPubKeyMan then check if the descriptors match
        DescriptorScriptPubKeyMan* spk_manager = dynamic_cast<DescriptorScriptPubKeyMan*>(spk_man_pair.second.get());
        if (spk_manager != nullptr && spk_manager->HasWalletDescriptor(desc)) {
            return spk_manager;
        }
    }

    return nullptr;
}

ScriptPubKeyMan* CWallet::AddWalletDescriptor(WalletDescriptor& desc, const FlatSigningProvider& signing_provider, const std::string& label, bool internal)
{
    if (!IsWalletFlagSet(WALLET_FLAG_DESCRIPTORS)) {
        WalletLogPrintf("Cannot add WalletDescriptor to a non-descriptor wallet\n");
        return nullptr;
    }

    LOCK(cs_wallet);
    auto spk_man = GetDescriptorScriptPubKeyMan(desc);
    if (spk_man) {
        WalletLogPrintf("Update existing descriptor: %s\n", desc.descriptor->ToString());
        spk_man->UpdateWalletDescriptor(desc);
    } else {
        auto new_spk_man = std::unique_ptr<DescriptorScriptPubKeyMan>(new DescriptorScriptPubKeyMan(*this, desc));
        spk_man = new_spk_man.get();

        // Save the descriptor to memory
        m_spk_managers[new_spk_man->GetID()] = std::move(new_spk_man);
    }

    // Add the private keys to the descriptor
    for (const auto& entry : signing_provider.keys) {
        const CKey& key = entry.second;
        spk_man->AddDescriptorKey(key, key.GetPubKey());
    }

    // Top up key pool, the manager will generate new scriptPubKeys internally
    if (!spk_man->TopUp()) {
        WalletLogPrintf("Could not top up scriptPubKeys\n");
        return nullptr;
    }

    // Apply the label if necessary
    // Note: we disable labels for ranged descriptors
    if (!desc.descriptor->IsRange()) {
        auto script_pub_keys = spk_man->GetScriptPubKeys();
        if (script_pub_keys.empty()) {
            WalletLogPrintf("Could not generate scriptPubKeys (cache is empty)\n");
            return nullptr;
        }

        CTxDestination dest;
        if (!internal && ExtractDestination(script_pub_keys.at(0), dest)) {
            SetAddressBook(dest, label, "receive");
        }
    }

    // Save the descriptor to DB
    spk_man->WriteDescriptor();

    return spk_man;
}

uint256 CTokenInfo::GetHash() const
{
    return SerializeHash(*this, SER_GETHASH, 0);
}

uint256 CTokenTx::GetHash() const
{
    return SerializeHash(*this, SER_GETHASH, 0);
}

uint256 CDelegationInfo::GetHash() const
{
    return SerializeHash(*this, SER_GETHASH, 0);
}

uint256 CSuperStakerInfo::GetHash() const
{
    return SerializeHash(*this, SER_GETHASH, 0);
}

bool CWallet::GetTokenTxDetails(const CTokenTx &wtx, uint256 &credit, uint256 &debit, std::string &tokenSymbol, uint8_t &decimals) const
{
    LOCK(cs_wallet);
    bool ret = false;

    for(auto it = mapToken.begin(); it != mapToken.end(); it++)
    {
        CTokenInfo info = it->second;
        if(wtx.strContractAddress == info.strContractAddress)
        {
            if(wtx.strSenderAddress == info.strSenderAddress)
            {
                debit = wtx.nValue;
                tokenSymbol = info.strTokenSymbol;
                decimals = info.nDecimals;
                ret = true;
            }

            if(wtx.strReceiverAddress == info.strSenderAddress)
            {
                credit = wtx.nValue;
                tokenSymbol = info.strTokenSymbol;
                decimals = info.nDecimals;
                ret = true;
            }
        }
    }

    return ret;
}

bool CWallet::IsTokenTxMine(const CTokenTx &wtx) const
{
    LOCK(cs_wallet);
    bool ret = false;

    for(auto it = mapToken.begin(); it != mapToken.end(); it++)
    {
        CTokenInfo info = it->second;
        if(wtx.strContractAddress == info.strContractAddress)
        {
            if(wtx.strSenderAddress == info.strSenderAddress || 
                wtx.strReceiverAddress == info.strSenderAddress)
            {
                ret = true;
            }
        }
    }

    return ret;
}

bool CWallet::RemoveTokenEntry(const uint256 &tokenHash, bool fFlushOnClose)
{
    LOCK(cs_wallet);

    WalletBatch batch(GetDatabase(), fFlushOnClose);

    bool fFound = false;

    std::map<uint256, CTokenInfo>::iterator it = mapToken.find(tokenHash);
    if(it!=mapToken.end())
    {
        fFound = true;
    }

    if(fFound)
    {
        // Remove from disk
        if (!batch.EraseToken(tokenHash))
            return false;

        mapToken.erase(it);

        NotifyTokenChanged(this, tokenHash, CT_DELETED);

        // Refresh token tx
        for(auto it = mapTokenTx.begin(); it != mapTokenTx.end(); it++)
        {
            uint256 tokenTxHash = it->second.GetHash();
            NotifyTokenTransactionChanged(this, tokenTxHash, CT_UPDATED);
        }
    }

    LogPrintf("RemoveTokenEntry %s\n", tokenHash.ToString());

    return true;
}

bool CWallet::CleanTokenTxEntries(bool fFlushOnClose)
{
    LOCK(cs_wallet);

    // Open db
    WalletBatch batch(GetDatabase(), fFlushOnClose);

    // Get all token transaction hashes
    std::vector<uint256> tokenTxHashes;
    for(auto it = mapTokenTx.begin(); it != mapTokenTx.end(); it++)
    {
        tokenTxHashes.push_back(it->first);
    }

    // Remove existing entries
    for(size_t i = 0; i < tokenTxHashes.size(); i++)
    {
        // Get the I entry
        uint256 hashTxI = tokenTxHashes[i];
        auto itTxI = mapTokenTx.find(hashTxI);
        if(itTxI == mapTokenTx.end()) continue;
        CTokenTx tokenTxI = itTxI->second;

        for(size_t j = 0; j < tokenTxHashes.size(); j++)
        {
            // Skip the same entry
            if(i == j) continue;

            // Get the J entry
            uint256 hashTxJ = tokenTxHashes[j];
            auto itTxJ = mapTokenTx.find(hashTxJ);
            if(itTxJ == mapTokenTx.end()) continue;
            CTokenTx tokenTxJ = itTxJ->second;

            // Compare I and J entries
            if(tokenTxI.strContractAddress != tokenTxJ.strContractAddress) continue;
            if(tokenTxI.strSenderAddress != tokenTxJ.strSenderAddress) continue;
            if(tokenTxI.strReceiverAddress != tokenTxJ.strReceiverAddress) continue;
            if(tokenTxI.blockHash != tokenTxJ.blockHash) continue;
            if(tokenTxI.blockNumber != tokenTxJ.blockNumber) continue;
            if(tokenTxI.transactionHash != tokenTxJ.transactionHash) continue;

            // Delete the lower entry from disk
            size_t nLower = uintTou256(tokenTxI.nValue) < uintTou256(tokenTxJ.nValue) ? i : j;
            auto itTx = nLower == i ? itTxI : itTxJ;
            uint256 hashTx = nLower == i ? hashTxI : hashTxJ;

            if (!batch.EraseTokenTx(hashTx))
                return false;

            mapTokenTx.erase(itTx);

            NotifyTokenTransactionChanged(this, hashTx, CT_DELETED);

            break;
        }
    }

    return true;
}

bool CWallet::SetContractBook(const std::string &strAddress, const std::string &strName, const std::string &strAbi)
{
    bool fUpdated = false;
    {
        LOCK(cs_wallet); // mapContractBook
        auto mi = mapContractBook.find(strAddress);
        fUpdated = mi != mapContractBook.end();
        mapContractBook[strAddress].name = strName;
        mapContractBook[strAddress].abi = strAbi;
    }

    NotifyContractBookChanged(this, strAddress, strName, strAbi, (fUpdated ? CT_UPDATED : CT_NEW) );

    WalletBatch batch(GetDatabase(), true);
    bool ret = batch.WriteContractData(strAddress, "name", strName);
    ret &= batch.WriteContractData(strAddress, "abi", strAbi);
    return ret;
}

bool CWallet::DelContractBook(const std::string &strAddress)
{
    {
        LOCK(cs_wallet); // mapContractBook
        mapContractBook.erase(strAddress);
    }

    NotifyContractBookChanged(this, strAddress, "", "", CT_DELETED);

    WalletBatch batch(GetDatabase(), true);
    bool ret = batch.EraseContractData(strAddress, "name");
    ret &= batch.EraseContractData(strAddress, "abi");
    return ret;
}

bool CWallet::LoadContractData(const std::string &address, const std::string &key, const std::string &value)
{
    bool ret = true;
    if(key == "name")
    {
        mapContractBook[address].name = value;
    }
    else if(key == "abi")
    {
        mapContractBook[address].abi = value;
    }
    else
    {
        ret = false;
    }
    return ret;
}

bool CWallet::LoadDelegation(const CDelegationInfo &delegation)
{
    uint256 hash = delegation.GetHash();
    mapDelegation[hash] = delegation;

    return true;
}

bool CWallet::AddDelegationEntry(const CDelegationInfo& delegation, bool fFlushOnClose)
{
    LOCK(cs_wallet);

    WalletBatch batch(GetDatabase(), fFlushOnClose);

    uint256 hash = delegation.GetHash();

    bool fInsertedNew = true;

    std::map<uint256, CDelegationInfo>::iterator it = mapDelegation.find(hash);
    if(it!=mapDelegation.end())
    {
        fInsertedNew = false;
    }

    // Write to disk
    CDelegationInfo wdelegation = delegation;
    if(!fInsertedNew)
    {
        wdelegation.nCreateTime = chain().getAdjustedTime();
    }
    else
    {
        wdelegation.nCreateTime = it->second.nCreateTime;
    }

    if (!batch.WriteDelegation(wdelegation))
        return false;

    mapDelegation[hash] = wdelegation;

    NotifyDelegationChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

    if(fInsertedNew)
    {
        LogPrintf("AddDelegationEntry %s\n", wdelegation.GetHash().ToString());
    }

    return true;
}

bool CWallet::RemoveDelegationEntry(const uint256& delegationHash, bool fFlushOnClose)
{
    LOCK(cs_wallet);

    WalletBatch batch(GetDatabase(), fFlushOnClose);

    bool fFound = false;

    std::map<uint256, CDelegationInfo>::iterator it = mapDelegation.find(delegationHash);
    if(it!=mapDelegation.end())
    {
        fFound = true;
    }

    if(fFound)
    {
        // Remove from disk
        if (!batch.EraseDelegation(delegationHash))
            return false;

        mapDelegation.erase(it);

        NotifyDelegationChanged(this, delegationHash, CT_DELETED);
    }

    LogPrintf("RemoveDelegationEntry %s\n", delegationHash.ToString());

    return true;
}

bool CWallet::LoadSuperStaker(const CSuperStakerInfo &superStaker)
{
    uint256 hash = superStaker.GetHash();
    mapSuperStaker[hash] = superStaker;

    return true;
}

bool CWallet::AddSuperStakerEntry(const CSuperStakerInfo& superStaker, bool fFlushOnClose)
{
    LOCK(cs_wallet);

    WalletBatch batch(GetDatabase(), fFlushOnClose);

    uint256 hash = superStaker.GetHash();

    bool fInsertedNew = true;

    std::map<uint256, CSuperStakerInfo>::iterator it = mapSuperStaker.find(hash);
    if(it!=mapSuperStaker.end())
    {
        fInsertedNew = false;
    }

    // Write to disk
    CSuperStakerInfo wsuperStaker = superStaker;
    if(!fInsertedNew)
    {
        wsuperStaker.nCreateTime = chain().getAdjustedTime();
    }
    else
    {
        wsuperStaker.nCreateTime = it->second.nCreateTime;
    }

    if (!batch.WriteSuperStaker(wsuperStaker))
        return false;

    mapSuperStaker[hash] = wsuperStaker;

    NotifySuperStakerChanged(this, hash, fInsertedNew ? CT_NEW : CT_UPDATED);

    if(fInsertedNew)
    {
        LogPrintf("AddSuperStakerEntry %s\n", wsuperStaker.GetHash().ToString());
    }
    else
    {
        fUpdatedSuperStaker = true;
    }

    return true;
}

bool CWallet::RemoveSuperStakerEntry(const uint256& superStakerHash, bool fFlushOnClose)
{
    LOCK(cs_wallet);

    WalletBatch batch(GetDatabase(), fFlushOnClose);

    bool fFound = false;

    std::map<uint256, CSuperStakerInfo>::iterator it = mapSuperStaker.find(superStakerHash);
    if(it!=mapSuperStaker.end())
    {
        fFound = true;
    }

    if(fFound)
    {
        // Remove from disk
        if (!batch.EraseSuperStaker(superStakerHash))
            return false;

        mapSuperStaker.erase(it);

        NotifySuperStakerChanged(this, superStakerHash, CT_DELETED);
    }

    LogPrintf("RemoveSuperStakerEntry %s\n", superStakerHash.ToString());

    return true;
}

void CWallet::StakeYodys(bool fStake)
{
    ::StakeYodys(fStake, this, stakeThread);
}

void CWallet::StartStake()
{
    if(IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS))
    {
        m_enabled_staking = !m_ledger_id.empty() && YodyLedger::instance().toolExists();
    }
    else
    {
        m_enabled_staking = true;
    }

    StakeYodys(true);
}

void CWallet::StopStake()
{
    if(!stakeThread)
    {
        if(m_enabled_staking)
            m_enabled_staking = false;
    }
    else
    {
        m_stop_staking_thread = true;
        m_enabled_staking = false;
        StakeYodys(false);
        stakeThread = 0;
        m_stop_staking_thread = false;
    }
}

bool CWallet::IsStakeClosing()
{
    return chain().shutdownRequested() || m_stop_staking_thread;
}

void CWallet::updateDelegationsStaker(const std::map<uint160, Delegation> &delegations_staker)
{
    LOCK(cs_wallet);

    // Notify for updated and deleted delegation items
    for (std::map<uint160, Delegation>::iterator it=m_delegations_staker.begin(); it!=m_delegations_staker.end();)
    {
        uint160 addressDelegate = it->first;
        std::map<uint160, Delegation>::const_iterator delegation = delegations_staker.find(addressDelegate);
        if(delegation == delegations_staker.end())
        {
            it = m_delegations_staker.erase(it);
            m_delegations_weight.erase(addressDelegate);
            NotifyDelegationsStakerChanged(this, addressDelegate, CT_DELETED);
        }
        else
        {
            if(delegation->second != it->second)
            {
                it->second = delegation->second;
                NotifyDelegationsStakerChanged(this, addressDelegate, CT_UPDATED);
            }
            it++;
        }
    }

    // Notify for new delegation items
    for (std::map<uint160, Delegation>::const_iterator it=delegations_staker.begin(); it!=delegations_staker.end(); it++)
    {
        if(m_delegations_staker.find(it->first) == m_delegations_staker.end())
        {
            m_delegations_staker[it->first] = it->second;
            NotifyDelegationsStakerChanged(this, it->first, CT_NEW);
        }
    }
}

void CWallet::updateDelegationsWeight(const std::map<uint160, CAmount>& delegations_weight)
{
    LOCK(cs_wallet);

    for (std::map<uint160, CAmount>::const_iterator mi = delegations_weight.begin(); mi != delegations_weight.end(); mi++)
    {
        bool updated = true;
        uint160 delegate = mi->first;
        CAmount weight = mi->second;
        std::map<uint160, CAmount>::iterator it = m_delegations_weight.find(delegate);
        if(it != m_delegations_weight.end())
        {
            if(it->second == weight)
            {
                updated = false;
            }
        }

        m_delegations_weight[delegate] = weight;

        if(updated && m_delegations_staker.find(delegate) != m_delegations_staker.end())
        {
            NotifyDelegationsStakerChanged(this, delegate, CT_UPDATED);
        }
    }

    for (std::map<uint256, CSuperStakerInfo>::iterator mi = mapSuperStaker.begin(); mi != mapSuperStaker.end(); mi++)
    {
        uint256 hash = mi->first;
        NotifySuperStakerChanged(this, hash, CT_UPDATED);
    }
}

uint64_t CWallet::GetSuperStakerWeight(const uint160 &staker) const
{
    LOCK(cs_wallet);

    uint64_t nWeight = 0;
    auto iterator = m_have_coin_superstaker.find(staker);
    if (iterator != m_have_coin_superstaker.end() && iterator->second)
    {
        for (std::map<uint160, Delegation>::const_iterator it=m_delegations_staker.begin(); it!=m_delegations_staker.end(); it++)
        {
            if(it->second.staker == staker)
            {
                uint160 delegate = it->first;
                std::map<uint160, CAmount>::const_iterator mi = m_delegations_weight.find(delegate);
                if(mi != m_delegations_weight.end())
                {
                    nWeight += mi->second;
                }
            }
        }
    }

    return nWeight;
}

bool CWallet::GetSuperStaker(CSuperStakerInfo &info, const uint160 &stakerAddress) const
{
    LOCK(cs_wallet);

    for (std::map<uint256, CSuperStakerInfo>::const_iterator it=mapSuperStaker.begin(); it!=mapSuperStaker.end(); it++)
    {
        if(it->second.stakerAddress == stakerAddress)
        {
            info = it->second;
            return true;
        }
    }

    return false;
}

void CWallet::GetStakerAddressBalance(const PKHash &staker, CAmount &balance, CAmount &stake, CAmount& weight) const
{
    AssertLockHeld(cs_wallet);

    balance = 0;
    stake = 0;
    weight = 0;
    int nHeight = GetLastBlockHeight() + 1;
    int coinbaseMaturity = Params().GetConsensus().CoinbaseMaturity(nHeight);
    std::map<COutPoint, uint32_t> immatureStakes = chain().getImmatureStakes();
    for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        const uint256& wtxid = it->first;
        const CWalletTx* pcoin = &(*it).second;
        int nDepth = pcoin->GetDepthInMainChain();

        if (nDepth < 1)
            continue;

        if(pcoin->m_confirm.hashBlock.IsNull())
            continue;
        bool fHasProofOfDelegation = pcoin->m_confirm.hasDelegation;

        bool isImature = pcoin->GetBlocksToMaturity() == 0;
        for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++)
        {
            bool OK = false;
            PKHash keyId = ExtractPublicKeyHash(pcoin->tx->vout[i].scriptPubKey, &OK);
            if(OK && keyId == staker)
            {
                isminetype mine = IsMine(pcoin->tx->vout[i]);
                  if (!(IsSpent(wtxid, i)) && mine != ISMINE_NO &&
                    !IsLockedCoin((*it).first, i) && (pcoin->tx->vout[i].nValue > 0))
                {
                      CAmount nValue = pcoin->tx->vout[i].nValue;
                      if(isImature)
                      {
                          balance += nValue;
                          if(nDepth >= coinbaseMaturity && nValue >= DEFAULT_STAKING_MIN_UTXO_VALUE)
                          {
                              COutPoint prevout = COutPoint(pcoin->tx->GetHash(), i);
                              if(immatureStakes.find(prevout) == immatureStakes.end())
                              {
                                  weight += nValue;
                              }
                          }
                      }
                      else if(pcoin->IsCoinStake() && fHasProofOfDelegation)
                      {
                          stake += nValue;
                      }
                }
            }
        }
    }
}

void CWallet::updateHaveCoinSuperStaker(const std::set<std::pair<const CWalletTx *, unsigned int> > &setCoins)
{
    LOCK(cs_wallet);
    m_have_coin_superstaker.clear();

    COutPoint prevout;
    CAmount nValueRet = 0;
    for (const auto& entry : mapSuperStaker) {
        if(GetCoinSuperStaker(setCoins, PKHash(entry.second.stakerAddress), prevout, nValueRet))
        {
            m_have_coin_superstaker[entry.second.stakerAddress] = true;
        }
    }
}

void CWallet::UpdateMinerStakeCache(bool fStakeCache, const std::vector<COutPoint> &prevouts, CBlockIndex *pindexPrev )
{
    if(minerStakeCache.size() > prevouts.size() + 100){
        minerStakeCache.clear();
    }

    if(fStakeCache)
    {
        for(const COutPoint &prevoutStake : prevouts)
        {
            boost::this_thread::interruption_point();
            CacheKernel(minerStakeCache, prevoutStake, pindexPrev, chain().getCoinsTip());
        }
        if(!fHasMinerStakeCache) fHasMinerStakeCache = true;
    }
}

void CWallet::CleanCoinStake()
{
    LOCK(cs_wallet);
    // Search the coinstake transactions and abandon transactions that are not confirmed in the blocks
    for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        const CWalletTx* wtx = &(*it).second;
        if (wtx && wtx->m_confirm.hashBlock.IsNull() && wtx->m_confirm.nIndex <= 0)
        {
            // Wallets need to refund inputs when disconnecting coinstake
            const CTransaction& tx = *(wtx->tx);
            if (tx.IsCoinStake() && IsFromMe(tx) && !wtx->isAbandoned())
            {
                WalletLogPrintf("%s: Revert coinstake tx %s\n", __func__, wtx->GetHash().ToString());
                DisableTransaction(tx);
            }
        }
    }
}

void CWallet::AvailableCoinsForStaking(const std::vector<uint256>& maturedTx, size_t from, size_t to, const std::map<COutPoint, uint32_t>& immatureStakes, std::vector<std::pair<const CWalletTx *, unsigned int> >& vCoins, std::map<COutPoint, CScriptCache>* insertScriptCache) const
{
    for(size_t i = from; i < to; i++)
    {
        std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(maturedTx[i]);
        if(it == mapWallet.end()) continue;
        const uint256& wtxid = it->first;
        const CWalletTx* pcoin = &(*it).second;
        for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
            isminetype mine = IsMine(pcoin->tx->vout[i]);
            if (!(IsSpent(wtxid, i)) && mine != ISMINE_NO &&
                !IsLockedCoin((*it).first, i) && (pcoin->tx->vout[i].nValue > 0) &&
                // Check if the staking coin is dust
                pcoin->tx->vout[i].nValue >= m_staker_min_utxo_size)
            {
                // Get the script data for the coin
                COutPoint prevout = COutPoint(pcoin->GetHash(), i);
                const CScriptCache& scriptCache = GetScriptCache(prevout, pcoin->tx->vout[i].scriptPubKey, insertScriptCache);

                // Check that the script is not a contract script
                if(scriptCache.contract || !scriptCache.keyIdOk)
                    continue;

                // Check that the address is not delegated to other staker
                if(m_my_delegations.find(scriptCache.keyId) != m_my_delegations.end())
                    continue;

                // Check prevout maturity
                if(immatureStakes.find(prevout) == immatureStakes.end())
                {
                    // Check if script is spendable
                    bool spendable = ((mine & ISMINE_SPENDABLE) != ISMINE_NO) || (((mine & ISMINE_WATCH_ONLY) != ISMINE_NO) && scriptCache.solvable);
                    if(spendable)
                        vCoins.push_back(std::make_pair(pcoin, i));
                }
            }
        }
    }
}

bool CWallet::SelectCoinsForStaking(CAmount &nTargetValue, std::set<std::pair<const CWalletTx *, unsigned int> > &setCoinsRet, CAmount &nValueRet) const
{
    std::vector<std::pair<const CWalletTx *, unsigned int> > vCoins;
    vCoins.clear();

    int nHeight = GetLastBlockHeight() + 1;
    int coinbaseMaturity = Params().GetConsensus().CoinbaseMaturity(nHeight);
    std::map<COutPoint, uint32_t> immatureStakes = chain().getImmatureStakes();
    std::vector<uint256> maturedTx;
    const bool include_watch_only = GetLegacyScriptPubKeyMan() && IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    const isminetype is_mine_filter = include_watch_only ? ISMINE_WATCH_ONLY : ISMINE_SPENDABLE;
    for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        // Check the cached data for available coins for the tx
        const CWalletTx* pcoin = &(*it).second;
        const CAmount tx_credit_mine{pcoin->GetAvailableCredit(/* fUseCache */ true, is_mine_filter | ISMINE_NO)};
        if(tx_credit_mine == 0)
            continue;

        const uint256& wtxid = it->first;
        int nDepth = pcoin->GetDepthInMainChain();

        if (nDepth < 1)
            continue;

        if (nDepth < coinbaseMaturity)
            continue;

        if (pcoin->GetBlocksToMaturity() > 0)
            continue;

        maturedTx.push_back(wtxid);
    }

    size_t listSize = maturedTx.size();
    int numThreads = std::min(m_num_threads, (int)listSize);
    if(numThreads < 2)
    {
        AvailableCoinsForStaking(maturedTx, 0, listSize, immatureStakes, vCoins, nullptr);
    }
    else
    {
        size_t chunk = listSize / numThreads;
        for(int i = 0; i < numThreads; i++)
        {
            size_t from = i * chunk;
            size_t to = i == (numThreads -1) ? listSize : from + chunk;
            threads.create_thread([this, from, to, &maturedTx, &immatureStakes, &vCoins]{
                std::vector<std::pair<const CWalletTx *, unsigned int> > tmpCoins;
                std::map<COutPoint, CScriptCache> tmpInsertScriptCache;
                AvailableCoinsForStaking(maturedTx, from, to, immatureStakes, tmpCoins, &tmpInsertScriptCache);

                LOCK(cs_worker);
                vCoins.insert(vCoins.end(), tmpCoins.begin(), tmpCoins.end());
                if((int32_t)prevoutScriptCache.size() > m_staker_max_utxo_script_cache)
                {
                    prevoutScriptCache.clear();
                }
                prevoutScriptCache.insert(tmpInsertScriptCache.begin(), tmpInsertScriptCache.end());
            });
        }
        threads.join_all();
    }

    setCoinsRet.clear();
    nValueRet = 0;

    for(const std::pair<const CWalletTx*,unsigned int> &output : vCoins)
    {
        const CWalletTx *pcoin = output.first;
        int i = output.second;

        // Stop if we've chosen enough inputs
        if (nValueRet >= nTargetValue)
            break;

        int64_t n = pcoin->tx->vout[i].nValue;

        std::pair<int64_t,std::pair<const CWalletTx*,unsigned int> > coin = std::make_pair(n,std::make_pair(pcoin, i));

        if (n >= nTargetValue)
        {
            // If input value is greater or equal to target then simply insert
            // it into the current subset and exit
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
            break;
        }
        else if (n < nTargetValue + CENT)
        {
            setCoinsRet.insert(coin.second);
            nValueRet += coin.first;
        }
    }

    return true;
}

bool CWallet::AvailableDelegateCoinsForStaking(const std::vector<uint160>& delegations, size_t from, size_t to, int32_t height, const std::map<COutPoint, uint32_t>& immatureStakes,  const std::map<uint256, CSuperStakerInfo>& mapStakers, std::vector<std::pair<COutPoint,CAmount>>& vUnsortedDelegateCoins, std::map<uint160, CAmount> &mDelegateWeight) const
{
    for(size_t i = from; i < to; i++)
    {
        std::map<uint160, Delegation>::const_iterator it = m_delegations_staker.find(delegations[i]);
        if(it == m_delegations_staker.end()) continue;

        const PKHash& keyid = PKHash(it->first);
        const Delegation* delegation = &(*it).second;

        // Set default delegate stake weight
        CAmount weight = 0;
        mDelegateWeight[it->first] = weight;

        // Get super staker custom configuration
        CAmount staking_min_utxo_value = m_staking_min_utxo_value;
        uint8_t staking_min_fee = m_staking_min_fee;
        for (std::map<uint256, CSuperStakerInfo>::const_iterator it=mapStakers.begin(); it!=mapStakers.end(); it++)
        {
            if(it->second.stakerAddress == delegation->staker)
            {
                CSuperStakerInfo info = it->second;
                if(info.fCustomConfig)
                {
                    staking_min_utxo_value = info.nMinDelegateUtxo;
                    staking_min_fee = info.nMinFee;
                }
            }
        }

        // Check for min staking fee
        if(delegation->fee < staking_min_fee)
            continue;

        // Decode address
        uint256 hashBytes;
        int type = 0;
        if (!DecodeIndexKey(EncodeDestination(keyid), hashBytes, type)) {
            return error("Invalid address");
        }

        // Get address utxos
        std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> > unspentOutputs;
        if (!GetAddressUnspent(hashBytes, type, unspentOutputs)) {
            throw error("No information available for address");
        }

        // Add the utxos to the list if they are mature and at least the minimum value
        int coinbaseMaturity = Params().GetConsensus().CoinbaseMaturity(height + 1);
        for (std::vector<std::pair<CAddressUnspentKey, CAddressUnspentValue> >::const_iterator i=unspentOutputs.begin(); i!=unspentOutputs.end(); i++) {

            int nDepth = height - i->second.blockHeight + 1;
            if (nDepth < coinbaseMaturity)
                continue;

            if(i->second.satoshis < staking_min_utxo_value)
                continue;

            COutPoint prevout = COutPoint(i->first.txhash, i->first.index);
            if(immatureStakes.find(prevout) == immatureStakes.end())
            {
                vUnsortedDelegateCoins.push_back(std::make_pair(prevout, i->second.satoshis));
                weight+= i->second.satoshis;
            }
        }

        // Update delegate stake weight
        mDelegateWeight[it->first] = weight;
    }

    return true;
}

bool CWallet::SelectDelegateCoinsForStaking(std::vector<COutPoint> &setDelegateCoinsRet, std::map<uint160, CAmount> &mDelegateWeight) const
{
    AssertLockHeld(cs_wallet);

    setDelegateCoinsRet.clear();

    std::vector<std::pair<COutPoint,CAmount>> vUnsortedDelegateCoins;

    int32_t const height = chain().getHeight().value_or(-1);
    if (height == -1) {
        return error("Invalid blockchain height");
    }

    std::map<COutPoint, uint32_t> immatureStakes = chain().getImmatureStakes();
    std::map<uint256, CSuperStakerInfo> mapStakers = mapSuperStaker;

    std::vector<uint160> delegations;
    for (std::map<uint160, Delegation>::const_iterator it = m_delegations_staker.begin(); it != m_delegations_staker.end(); ++it)
    {
        delegations.push_back(it->first);
    }
    size_t listSize = delegations.size();
    int numThreads = std::min(m_num_threads, (int)listSize);
    bool ret = true;
    if(numThreads < 2)
    {
        ret = AvailableDelegateCoinsForStaking(delegations, 0, listSize, height, immatureStakes, mapStakers, vUnsortedDelegateCoins, mDelegateWeight);
    }
    else
    {
        size_t chunk = listSize / numThreads;
        for(int i = 0; i < numThreads; i++)
        {
            size_t from = i * chunk;
            size_t to = i == (numThreads -1) ? listSize : from + chunk;
            threads.create_thread([this, from, to, height, &delegations, &immatureStakes, &mapStakers, &ret, &vUnsortedDelegateCoins, &mDelegateWeight]{
                std::vector<std::pair<COutPoint,CAmount>> tmpUnsortedDelegateCoins;
                std::map<uint160, CAmount> tmpDelegateWeight;
                bool tmpRet = AvailableDelegateCoinsForStaking(delegations, from, to, height, immatureStakes, mapStakers, tmpUnsortedDelegateCoins, tmpDelegateWeight);

                LOCK(cs_worker);
                ret &= tmpRet;
                vUnsortedDelegateCoins.insert(vUnsortedDelegateCoins.end(), tmpUnsortedDelegateCoins.begin(), tmpUnsortedDelegateCoins.end());
                mDelegateWeight.insert(tmpDelegateWeight.begin(), tmpDelegateWeight.end());
            });
        }
        threads.join_all();
    }

    std::sort(vUnsortedDelegateCoins.begin(), vUnsortedDelegateCoins.end(), valueUtxoSort);

    for(auto utxo : vUnsortedDelegateCoins){
        setDelegateCoinsRet.push_back(utxo.first);
    }

    vUnsortedDelegateCoins.clear();

    return ret;
}

void CWallet::AvailableAddress(const std::vector<uint256> &maturedTx, size_t from, size_t to, std::map<uint160, bool> &mapAddress, std::map<COutPoint, CScriptCache> *insertScriptCache) const
{
    for(size_t i = from; i < to; i++)
    {
        std::map<uint256, CWalletTx>::const_iterator it = mapWallet.find(maturedTx[i]);
        if(it == mapWallet.end()) continue;
        const uint256& wtxid = it->first;
        const CWalletTx* pcoin = &(*it).second;
        for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
            isminetype mine = IsMine(pcoin->tx->vout[i]);
            if (!(IsSpent(wtxid, i)) && mine != ISMINE_NO &&
                !IsLockedCoin((*it).first, i) && (pcoin->tx->vout[i].nValue > 0) &&
                // Check if the staking coin is dust
                pcoin->tx->vout[i].nValue >= m_staker_min_utxo_size)
            {
                // Get the script data for the coin
                COutPoint prevout = COutPoint(pcoin->GetHash(), i);
                const CScriptCache& scriptCache = GetScriptCache(prevout, pcoin->tx->vout[i].scriptPubKey, insertScriptCache);

                // Check that the script is not a contract script
                if(scriptCache.contract || !scriptCache.keyIdOk)
                    continue;

                bool spendable = ((mine & ISMINE_SPENDABLE) != ISMINE_NO) || (((mine & ISMINE_WATCH_ONLY) != ISMINE_NO) && scriptCache.solvable);
                if(spendable)
                {
                    if(mapAddress.find(scriptCache.keyId) == mapAddress.end())
                    {
                        mapAddress[scriptCache.keyId] = true;
                    }
                }
            }
        }
    }
}

void CWallet::SelectAddress(std::map<uint160, bool> &mapAddress) const
{
    std::vector<uint256> maturedTx;
    const bool include_watch_only = GetLegacyScriptPubKeyMan() && IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    const isminetype is_mine_filter = include_watch_only ? ISMINE_WATCH_ONLY : ISMINE_SPENDABLE;
    for (std::map<uint256, CWalletTx>::const_iterator it = mapWallet.begin(); it != mapWallet.end(); ++it)
    {
        // Check the cached data for available coins for the tx
        const CWalletTx* pcoin = &(*it).second;
        const CAmount tx_credit_mine{pcoin->GetAvailableCredit(/* fUseCache */ true, is_mine_filter | ISMINE_NO)};
        if(tx_credit_mine == 0)
            continue;

        const uint256& wtxid = it->first;
        int nDepth = pcoin->GetDepthInMainChain();

        if (nDepth < 1)
            continue;

        if (pcoin->GetBlocksToMaturity() > 0)
            continue;

        maturedTx.push_back(wtxid);
    }

    size_t listSize = maturedTx.size();
    int numThreads = std::min(m_num_threads, (int)listSize);
    if(numThreads < 2)
    {
        AvailableAddress(maturedTx, 0, listSize, mapAddress, nullptr);
    }
    else
    {
        size_t chunk = listSize / numThreads;
        for(int i = 0; i < numThreads; i++)
        {
            size_t from = i * chunk;
            size_t to = i == (numThreads -1) ? listSize : from + chunk;
            threads.create_thread([this, from, to, &maturedTx, &mapAddress]{
                std::map<uint160, bool> tmpAddresses;
                std::map<COutPoint, CScriptCache> tmpInsertScriptCache;
                AvailableAddress(maturedTx, from, to, tmpAddresses, &tmpInsertScriptCache);

                LOCK(cs_worker);
                mapAddress.insert(tmpAddresses.begin(), tmpAddresses.end());
                if((int32_t)prevoutScriptCache.size() > m_staker_max_utxo_script_cache)
                {
                    prevoutScriptCache.clear();
                }
                prevoutScriptCache.insert(tmpInsertScriptCache.begin(), tmpInsertScriptCache.end());
            });
        }
        threads.join_all();
    }
}

bool CWallet::HasPrivateKey(const CTxDestination& dest, const bool& fAllowWatchOnly)
{
    CScript script = GetScriptForDestination(dest);
    isminetype mine = IsMine(script);
    if(!mine) return false;
    std::unique_ptr<SigningProvider> provider = GetSolvingProvider(script);
    bool solvable = provider ? IsSolvable(*provider, script) : false;
    bool spendable = ((mine & ISMINE_SPENDABLE) != ISMINE_NO) || (((mine & ISMINE_WATCH_ONLY) != ISMINE_NO) && (fAllowWatchOnly && solvable));
    return spendable;
}

CKeyID CWallet::GetKeyForDestination(const CTxDestination& dest)
{
    CScript script = GetScriptForDestination(dest);
    std::unique_ptr<SigningProvider> provider = GetSolvingProvider(script);
    if(!provider) provider = std::make_unique<FlatSigningProvider>();
    return ::GetKeyForDestination(*provider, dest);
}

bool CWallet::GetPubKey(const PKHash& pkhash, CPubKey& pubkey) const
{
    CScript script = GetScriptForDestination(pkhash);
    std::unique_ptr<SigningProvider> provider = GetSolvingProvider(script);
    if(provider)
    {
        return provider->GetPubKey(ToKeyID(pkhash), pubkey);
    }

    return false;
}

bool CWallet::GetKeyOrigin(const PKHash& pkhash, KeyOriginInfo& info) const
{
    CScript script = GetScriptForDestination(pkhash);
    std::unique_ptr<SigningProvider> provider = GetSolvingProvider(script);
    if(provider)
    {
        return provider->GetKeyOrigin(ToKeyID(pkhash), info);
    }

    return false;
}

bool CWallet::GetSenderDest(const CTransaction &tx, CTxDestination &txSenderDest, bool sign) const
{
    // Initialize variables
    CScript senderPubKey;

    // Get sender destination
    if(tx.HasOpSender())
    {
        // Get destination from the outputs
        for(CTxOut out : tx.vout)
        {
            if(out.scriptPubKey.HasOpSender())
            {
                if(sign)
                {
                    ExtractSenderData(out.scriptPubKey, &senderPubKey, 0);
                }
                else
                {
                    GetSenderPubKey(out.scriptPubKey, senderPubKey);
                }
                break;
            }
        }
    }
    else
    {
        // Get destination from the inputs
        if(tx.vin.size() > 0 && mapWallet.find(tx.vin[0].prevout.hash) != mapWallet.end())
        {
            senderPubKey = mapWallet.at(tx.vin[0].prevout.hash).tx->vout[tx.vin[0].prevout.n].scriptPubKey;
        }
    }

    // Extract destination from script
    return ExtractDestination(senderPubKey, txSenderDest);
}

bool CWallet::GetHDKeyPath(const CTxDestination &dest, std::string &hdkeypath) const
{
    CScript scriptPubKey = GetScriptForDestination(dest);
    ScriptPubKeyMan* spk_man = GetScriptPubKeyMan(scriptPubKey);
    if (spk_man) {
        if (const std::unique_ptr<CKeyMetadata> meta = spk_man->GetMetadata(dest)) {
            if (meta->has_key_origin) {
                hdkeypath = WriteHDKeypath(meta->key_origin.path);
                return true;
            }
        }
    }

    return false;
}
