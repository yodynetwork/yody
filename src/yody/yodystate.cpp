#include <sstream>
#include <util/system.h>
#include <validation.h>
#include <chainparams.h>
#include <script/script.h>
#include <yody/yodystate.h>
#include <libevm/VMFace.h>
#include <validation.h>

using namespace std;
using namespace dev;
using namespace dev::eth;

YodyState::YodyState(u256 const& _accountStartNonce, OverlayDB const& _db, const string& _path, BaseState _bs) :
        State(_accountStartNonce, _db, _bs) {
            dbUTXO = YodyState::openDB(_path + "/yodyDB", sha3(rlp("")), WithExisting::Trust);
	        stateUTXO = SecureTrieDB<Address, OverlayDB>(&dbUTXO);
}

YodyState::YodyState() : dev::eth::State(dev::Invalid256, dev::OverlayDB(), dev::eth::BaseState::PreExisting) {
    dbUTXO = OverlayDB();
    stateUTXO = SecureTrieDB<Address, OverlayDB>(&dbUTXO);
}

ResultExecute YodyState::execute(EnvInfo const& _envInfo, SealEngineFace const& _sealEngine, YodyTransaction const& _t, CChain& _chain, Permanence _p, OnOpFunc const& _onOp){

    assert(_t.getVersion().toRaw() == VersionVM::GetEVMDefault().toRaw());

    addBalance(_t.sender(), _t.value() + (_t.gas() * _t.gasPrice()));
    newAddress = _t.isCreation() ? createYodyAddress(_t.getHashWith(), _t.getNVout()) : dev::Address();

    _sealEngine.deleteAddresses.insert({_t.sender(), _envInfo.author()});

    h256 oldStateRoot = rootHash();
    h256 oldUTXORoot = rootHashUTXO();
    bool voutLimit = false;

	auto onOp = _onOp;
#if ETH_VMTRACE
	if (isChannelVisible<VMTraceChannel>())
		onOp = Executive::simpleTrace(); // override tracer
#endif
	// Create and initialize the executive. This will throw fairly cheaply and quickly if the
	// transaction is bad in any way.
	Executive e(*this, _envInfo, _sealEngine);
	ExecutionResult res;
	e.setResultRecipient(res);

    CTransactionRef tx;
    u256 startGasUsed;
    const Consensus::Params& consensusParams = Params().GetConsensus();
    try{
        if (_t.isCreation() && _t.value())
            BOOST_THROW_EXCEPTION(CreateWithValue());

        e.initialize(_t);
        // OK - transaction looks valid - execute.
        startGasUsed = _envInfo.gasUsed();
        if (!e.execute()){
            e.go(onOp);
            if(_chain.Height() >= consensusParams.QIP7Height){
            	validateTransfersWithChangeLog();
            }
        } else {
            e.revert();
            throw Exception();
        }
        e.finalize();
        if (_p == Permanence::Reverted){
            m_cache.clear();
            cacheUTXO.clear();
            m_changeLog.clear();
            m_unchangedCacheEntries.clear();
        } else {
            deleteAccounts(_sealEngine.deleteAddresses);
            if(res.excepted == TransactionException::None){
                CondensingTX ctx(this, transfers, _t, _sealEngine.deleteAddresses);
                tx = MakeTransactionRef(ctx.createCondensingTX());
                if(ctx.reachedVoutLimit()){
                    voutLimit = true;
                    e.revert();
                    throw Exception();
                }
                std::unordered_map<dev::Address, Vin> vins = ctx.createVin(*tx);
                updateUTXO(vins);
            } else {
                printfErrorLog(res.excepted);
            }

            yody::commit(cacheUTXO, stateUTXO, m_cache);
            cacheUTXO.clear();
            bool removeEmptyAccounts = _envInfo.number() >= _sealEngine.chainParams().EIP158ForkBlock;
            commit(removeEmptyAccounts ? State::CommitBehaviour::RemoveEmptyAccounts : State::CommitBehaviour::KeepEmptyAccounts);
        }
    }
    catch(Exception const& _e){
        printfErrorLog(dev::eth::toTransactionException(_e));
        res.excepted = dev::eth::toTransactionException(_e);
        res.gasUsed = _t.gas();
        if(_chain.Height() < consensusParams.nFixUTXOCacheHFHeight  && _p != Permanence::Reverted){
            deleteAccounts(_sealEngine.deleteAddresses);
            commit(CommitBehaviour::RemoveEmptyAccounts);
        } else {
            m_cache.clear();
            cacheUTXO.clear();
        }
    }

    if(!_t.isCreation())
        res.newAddress = _t.receiveAddress();
    newAddress = dev::Address();
    transfers.clear();
    if(voutLimit){
        //use old and empty states to create virtual Out Of Gas exception
        LogEntries logs;
        u256 gas = _t.gas();
        ExecutionResult ex;
        ex.gasRefunded=0;
        ex.gasUsed=gas;
        ex.excepted=TransactionException();
        //create a refund tx to send back any coins that were suppose to be sent to the contract
        CMutableTransaction refund;
        if(_t.value() > 0) {
            refund.vin.push_back(CTxIn(h256Touint(_t.getHashWith()), _t.getNVout(), CScript() << OP_SPEND));
            //note, if sender was a non-standard tx, this will send the coins to pubkeyhash 0x00, effectively destroying the coins
            CScript script(CScript() << OP_DUP << OP_HASH160 << _t.sender().asBytes() << OP_EQUALVERIFY << OP_CHECKSIG);
            refund.vout.push_back(CTxOut(CAmount(_t.value().convert_to<uint64_t>()), script));
        }
        //make sure to use empty transaction if no vouts made
        return ResultExecute{ex, YodyTransactionReceipt(oldStateRoot, oldUTXORoot, gas, e.logs()), refund.vout.empty() ? CTransaction() : CTransaction(refund)};
    }else{
        return ResultExecute{res, YodyTransactionReceipt(rootHash(), rootHashUTXO(), startGasUsed + e.gasUsed(), e.logs()), tx ? *tx : CTransaction()};
    }
}

std::unordered_map<dev::Address, Vin> YodyState::vins() const // temp
{
    std::unordered_map<dev::Address, Vin> ret;
    for (auto& i: cacheUTXO)
        if (i.second.alive)
            ret[i.first] = i.second;
    auto addrs = addresses();
    for (auto& i : addrs){
        if (cacheUTXO.find(i.first) == cacheUTXO.end() && vin(i.first))
            ret[i.first] = *vin(i.first);
    }
    return ret;
}

void YodyState::transferBalance(dev::Address const& _from, dev::Address const& _to, dev::u256 const& _value) {
    subBalance(_from, _value);
    addBalance(_to, _value);
    if (_value > 0)
        transfers.push_back({_from, _to, _value});
}

Vin const* YodyState::vin(dev::Address const& _a) const
{
    return const_cast<YodyState*>(this)->vin(_a);
}

Vin* YodyState::vin(dev::Address const& _addr)
{
    auto it = cacheUTXO.find(_addr);
    if (it == cacheUTXO.end()){
        std::string stateBack = stateUTXO.at(_addr);
        if (stateBack.empty())
            return nullptr;
            
        dev::RLP state(stateBack);
        auto i = cacheUTXO.emplace(
            std::piecewise_construct,
            std::forward_as_tuple(_addr),
            std::forward_as_tuple(Vin{state[0].toHash<dev::h256>(), state[1].toInt<uint32_t>(), state[2].toInt<dev::u256>(), state[3].toInt<uint8_t>()})
        );
        return &i.first->second;
    }
    return &it->second;
}

// void YodyState::commit(CommitBehaviour _commitBehaviour)
// {
//     if (_commitBehaviour == CommitBehaviour::RemoveEmptyAccounts)
//         removeEmptyAccounts();

//     yody::commit(cacheUTXO, stateUTXO, m_cache);
//     cacheUTXO.clear();
        
//     m_touched += dev::eth::commit(m_cache, m_state);
//     m_changeLog.clear();
//     m_cache.clear();
//     m_unchangedCacheEntries.clear();
// }

void YodyState::kill(dev::Address _addr)
{
    // If the account is not in the db, nothing to kill.
    if (auto a = account(_addr))
        a->kill();
    if (auto v = vin(_addr))
        v->alive = 0;
}

void YodyState::addBalance(dev::Address const& _id, dev::u256 const& _amount)
{
    if (dev::eth::Account* a = account(_id))
    {
            // Log empty account being touched. Empty touched accounts are cleared
            // after the transaction, so this event must be also reverted.
            // We only log the first touch (not dirty yet), and only for empty
            // accounts, as other accounts does not matter.
            // TODO: to save space we can combine this event with Balance by having
            //       Balance and Balance+Touch events.
        if (!a->isDirty() && a->isEmpty())
            m_changeLog.emplace_back(dev::eth::Change::Touch, _id);

            // Increase the account balance. This also is done for value 0 to mark
            // the account as dirty. Dirty account are not removed from the cache
            // and are cleared if empty at the end of the transaction.
        a->addBalance(_amount);
    }
    else
    {
        if(!addressInUse(newAddress) && newAddress != dev::Address()){
            const_cast<dev::Address&>(_id) = newAddress;
            newAddress = dev::Address();
        }
        createAccount(_id, {requireAccountStartNonce(), _amount});
    }

    if (_amount)
        m_changeLog.emplace_back(dev::eth::Change::Balance, _id, _amount);
}

void YodyState::deleteAccounts(std::set<dev::Address>& addrs){
    for(dev::Address addr : addrs){
        dev::eth::Account* acc = const_cast<dev::eth::Account*>(account(addr));
        if(acc)
            acc->kill();
        Vin* in = const_cast<Vin*>(vin(addr));
        if(in)
            in->alive = 0;
    }
}

void YodyState::updateUTXO(const std::unordered_map<dev::Address, Vin>& vins){
    for(auto& v : vins){
        Vin* vi = const_cast<Vin*>(vin(v.first));

        if(vi){
            vi->hash = v.second.hash;
            vi->nVout = v.second.nVout;
            vi->value = v.second.value;
            vi->alive = v.second.alive;
        } else if(v.second.alive > 0) {
            cacheUTXO[v.first] = v.second;
        }
    }
}

void YodyState::printfErrorLog(const dev::eth::TransactionException er){
    std::stringstream ss;
    ss << er;
    clog(dev::VerbosityWarning, "exec") << "VM exception:" << ss.str();
}

void YodyState::validateTransfersWithChangeLog(){
	ChangeLog tmpChangeLog = m_changeLog;
	std::vector<TransferInfo> validatedTransfers;

	for(const TransferInfo& ti : transfers){
		for(std::size_t i=0; i<tmpChangeLog.size(); ++i){
			//find the log entry for the receiver of the transfer
			if(tmpChangeLog[i].kind==Change::Balance && tmpChangeLog[i].address==ti.to && tmpChangeLog[i].value==ti.value){
				for(std::size_t j=0; j<tmpChangeLog.size(); ++j){
					//find the log entry for the sender of the transfer
					if(tmpChangeLog[j].kind==Change::Balance && tmpChangeLog[j].address==ti.from && tmpChangeLog[j].value==0-ti.value){
						// transfer is valid
						validatedTransfers.push_back(ti);
						// zero out found elements to avoid matching again
						tmpChangeLog[i].address=dev::Address(0);
						tmpChangeLog[j].address=dev::Address(0);
						break;
					}
				}
				break;
			}
		}
	}

	transfers=validatedTransfers;
}

void YodyState::deployDelegationsContract(){
    dev::Address delegationsAddress = uintToh160(Params().GetConsensus().delegationsAddress);
    if(!YodyState::addressInUse(delegationsAddress)){
        YodyState::createContract(delegationsAddress);
        YodyState::setCode(delegationsAddress, bytes{fromHex(DELEGATIONS_CONTRACT_CODE)}, YodyState::version(delegationsAddress));
        commit(CommitBehaviour::RemoveEmptyAccounts);
        db().commit();
    }
}
///////////////////////////////////////////////////////////////////////////////////////////
CTransaction CondensingTX::createCondensingTX(){
    selectionVin();
    calculatePlusAndMinus();
    if(!createNewBalances())
        return CTransaction();
    CMutableTransaction tx;
    tx.vin = createVins();;
    tx.vout = createVout();
    return !tx.vin.size() || !tx.vout.size() ? CTransaction() : CTransaction(tx);
}

std::unordered_map<dev::Address, Vin> CondensingTX::createVin(const CTransaction& tx){
    std::unordered_map<dev::Address, Vin> vins;
    for(auto& b : balances){
        if(b.first == transaction.sender())
            continue;

        if(b.second > 0){
            vins[b.first] = Vin{uintToh256(tx.GetHash()), nVouts[b.first], b.second, 1};
        } else {
            vins[b.first] = Vin{uintToh256(tx.GetHash()), 0, 0, 0};
        }
    }
    return vins;
}

void CondensingTX::selectionVin(){
    for(const TransferInfo& ti : transfers){
        if(!vins.count(ti.from)){
            if(auto a = state->vin(ti.from))
                vins[ti.from] = *a;
            if(ti.from == transaction.sender() && transaction.value() > 0){
                vins[ti.from] = Vin{transaction.getHashWith(), transaction.getNVout(), transaction.value(), 1};
            }
        }

        if(!vins.count(ti.to)){
            if(auto a = state->vin(ti.to))
                vins[ti.to] = *a;
        }
    }
}

void CondensingTX::calculatePlusAndMinus(){
    for(const TransferInfo& ti : transfers){
        if(!plusMinusInfo.count(ti.from)){
            plusMinusInfo[ti.from] = std::make_pair(0, ti.value);
        } else {
            plusMinusInfo[ti.from] = std::make_pair(plusMinusInfo[ti.from].first, plusMinusInfo[ti.from].second + ti.value);
        }

        if(!plusMinusInfo.count(ti.to)){
            plusMinusInfo[ti.to] = std::make_pair(ti.value, 0);
        } else {
            plusMinusInfo[ti.to] = std::make_pair(plusMinusInfo[ti.to].first + ti.value, plusMinusInfo[ti.to].second);
        }
    }
}

bool CondensingTX::createNewBalances(){
    for(auto& p : plusMinusInfo){
        dev::u256 balance = 0;
        if((vins.count(p.first) && vins[p.first].alive) || (!vins[p.first].alive && !checkDeleteAddress(p.first))){
            balance = vins[p.first].value;
        }
        balance += p.second.first;
        if(balance < p.second.second)
            return false;
        balance -= p.second.second;
        balances[p.first] = balance;
    }
    return true;
}

std::vector<CTxIn> CondensingTX::createVins(){
    std::vector<CTxIn> ins;
    for(auto& v : vins){
        if((v.second.value > 0 && v.second.alive) || (v.second.value > 0 && !vins[v.first].alive && !checkDeleteAddress(v.first)))
            ins.push_back(CTxIn(h256Touint(v.second.hash), v.second.nVout, CScript() << OP_SPEND));
    }
    return ins;
}

std::vector<CTxOut> CondensingTX::createVout(){
    size_t count = 0;
    std::vector<CTxOut> outs;
    for(auto& b : balances){
        if(b.second > 0){
            CScript script;
            auto* a = state->account(b.first);
            if(a && a->isAlive()){
                //create a no-exec contract output
                script = CScript() << valtype{0} << valtype{0} << valtype{0} << valtype{0} << b.first.asBytes() << OP_CALL;
            } else {
                script = CScript() << OP_DUP << OP_HASH160 << b.first.asBytes() << OP_EQUALVERIFY << OP_CHECKSIG;
            }
            outs.push_back(CTxOut(CAmount(b.second), script));
            nVouts[b.first] = count;
            count++;
        }
        if(count > MAX_CONTRACT_VOUTS){
            voutOverflow=true;
            return outs;
        }
    }
    return outs;
}

bool CondensingTX::checkDeleteAddress(dev::Address addr){
    return deleteAddresses.count(addr) != 0;
}
///////////////////////////////////////////////////////////////////////////////////////////
