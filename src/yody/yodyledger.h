#ifndef YODYLEDGER_H
#define YODYLEDGER_H

#include <string>
#include <vector>
#include <sync.h>

extern RecursiveMutex cs_ledger;

class YodyLedgerPriv;

struct LedgerDevice
{
    /// Device data
    std::string fingerprint;
    std::string serial_number;
    std::string type;
    std::string path;
    std::string error;
    std::string model;
    std::string code;
    std::string app_name;
};

/**
 * @brief The YodyLedger class Communicate with the yody ledger
 */
class YodyLedger {
    
public:
    /**
     * @brief YodyLedger Constructor
     */
    YodyLedger();

    /**
     * @brief ~YodyLedger Destructor
     */
    virtual ~YodyLedger();

    /**
     * @brief signCoinStake Sign proof of stake transaction
     * @param fingerprint Fingerprint of the ledger
     * @param psbt Proof of stake transaction
     * @return true/false
     */
    bool signCoinStake(const std::string& fingerprint, std::string& psbt);

    /**
     * @brief signBlockHeader Sign block header
     * @param fingerprint Fingerprint of the ledger
     * @param header Block header for the new block
     * @param path HD key path
     * @param vchSig Signature
     * @return true/false
     */
    bool signBlockHeader(const std::string& fingerprint, const std::string& header, const std::string& path, std::vector<unsigned char>& vchSig);

    /**
     * @brief isConnected Check if a device is connected
     * @param fingerprint Hardware wallet device fingerprint
     * @param stake Is stake app
     * @return success of the operation
     */
    bool isConnected(const std::string& fingerprint, bool stake);

    /**
     * @brief enumerate Enumerate hardware wallet devices
     * @param devices List of devices
     * @param stake Is stake app
     * @return success of the operation
     */
    bool enumerate(std::vector<LedgerDevice>& devices, bool stake);

    /**
     * @brief signTx Sign PSBT transaction
     * @param fingerprint Hardware wallet device fingerprint
     * @param psbt In/Out PSBT transaction
     * @return success of the operation
     */
    bool signTx(const std::string& fingerprint, std::string& psbt);

    /**
     * @brief signMessage Sign message
     * @param fingerprint Hardware wallet device fingerprint
     * @param message Message to sign
     * @param path HD key path
     * @param signature Signature of the message
     * @return success of the operation
     */
    bool signMessage(const std::string& fingerprint, const std::string& message, const std::string& path, std::string& signature);

    /**
     * @brief getKeyPool Get the key pool for a device
     * @param fingerprint Hardware wallet device fingerprint
     * @param type Type of output
     * @param path The derivation path, if empty it is used the default
     * @param internal Needed when the derivation path is specified, to determine if the address pool is for change addresses.
     * If path is empty both internal and external addresses are loaded into the pool, so the parameter is not used.
     * @param from Address list start
     * @param to Address list end
     * @param desc Address descriptors
     * @return success of the operation
     */
    bool getKeyPool(const std::string& fingerprint, int type, const std::string& path, bool internal, int from, int to, std::string& desc);

    /**
     * @brief errorMessage Get the last error message
     * @return Last error message
     */
    std::string errorMessage();

    /**
     * @brief toolExists Check if the hwi tool is accessible
     * @return true: accessible, false: not accessible
     */
    bool toolExists();

    /**
     * @brief derivationPath Get the default derivation path
     * @param type Type of output
     * @return Default derivation path
     */
    std::string derivationPath(int type);

    /**
     * @brief instance Get the ledger instance
     * @return
     */
    static YodyLedger &instance();

private:
    bool isStarted();
    void wait();

    bool beginSignTx(const std::string& fingerprint, std::string& psbt);
    bool endSignTx(const std::string& fingerprint, std::string& psbt);

    bool beginSignBlockHeader(const std::string& fingerprint, const std::string& header, const std::string& path, std::vector<unsigned char>& vchSig);
    bool endSignBlockHeader(const std::string& fingerprint, const std::string& header, const std::string& path, std::vector<unsigned char>& vchSig);

    bool beginEnumerate(std::vector<LedgerDevice>& devices);
    bool endEnumerate(std::vector<LedgerDevice>& devices, bool stake);

    bool beginSignMessage(const std::string& fingerprint, const std::string& message, const std::string& path, std::string &signature);
    bool endSignMessage(const std::string& fingerprint, const std::string& message, const std::string& path, std::string &signature);

    bool beginGetKeyPool(const std::string& fingerprint, int type, const std::string& path, bool internal, int from, int to, std::string& desc);
    bool endGetKeyPool(const std::string& fingerprint, int type, const std::string& path, bool internal,  int from, int to, std::string& desc);

private:
    YodyLedger(const YodyLedger&);
    YodyLedger& operator=(const YodyLedger&);
    YodyLedgerPriv* d;
};
#endif
