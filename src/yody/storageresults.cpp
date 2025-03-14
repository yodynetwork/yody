#include <yody/storageresults.h>
#include <util/convert.h>

StorageResults::StorageResults(std::string const& _path){
	path = _path + "/resultsDB";
    leveldb::Options options;
    options.create_if_missing = true;
    leveldb::Status status = leveldb::DB::Open(options, path, &db);
    assert(status.ok());
    LogPrintf("Opened LevelDB successfully\n");
}

StorageResults::~StorageResults()
{
    delete db;
    db = NULL;
}

void StorageResults::addResult(dev::h256 hashTx, std::vector<TransactionReceiptInfo>& result){
	m_cache_result.insert(std::make_pair(hashTx, result));
}

void StorageResults::clearCacheResult(){
    m_cache_result.clear();
}

void StorageResults::wipeResults(){
    LogPrintf("Wiping LevelDB in %s\n", path);
    bool opened = db;
    if (opened) {
        delete db;
    }
    leveldb::Status result = leveldb::DestroyDB(path, leveldb::Options());
    if (opened) {
        leveldb::Options options;
        options.create_if_missing = true;
        leveldb::Status status = leveldb::DB::Open(options, path, &db);
        assert(status.ok());
    }
}

void StorageResults::deleteResults(std::vector<CTransactionRef> const& txs){

    for(CTransactionRef tx : txs){
        dev::h256 hashTx = uintToh256(tx->GetHash());
        m_cache_result.erase(hashTx);

        std::string keyTemp = hashTx.hex();
	    leveldb::Slice key(keyTemp);
        leveldb::Status status = db->Delete(leveldb::WriteOptions(), key);
        assert(status.ok());
    }
}

std::vector<TransactionReceiptInfo> StorageResults::getResult(dev::h256 const& hashTx){
    std::vector<TransactionReceiptInfo> result;
	auto it = m_cache_result.find(hashTx);
	if (it == m_cache_result.end()){
		if(readResult(hashTx, result))
			m_cache_result.insert(std::make_pair(hashTx, result));
    } else {
		result = it->second;
    }
	return result;
}

void StorageResults::commitResults(){
    if(m_cache_result.size()){

        for (auto const& i: m_cache_result){
            std::string valueTemp;
            std::string keyTemp = i.first.hex();
            leveldb::Slice key(keyTemp);
            leveldb::Status status = db->Get(leveldb::ReadOptions(), key, &valueTemp);

            if(status.IsNotFound()){

                TransactionReceiptInfoSerialized tris;

                for(size_t j = 0; j < i.second.size(); j++){
                    tris.blockHashes.push_back(uintToh256(i.second[j].blockHash));
                    tris.blockNumbers.push_back(i.second[j].blockNumber);
                    tris.transactionHashes.push_back(uintToh256(i.second[j].transactionHash));
                    tris.transactionIndexes.push_back(i.second[j].transactionIndex);
                    tris.senders.push_back(i.second[j].from);
                    tris.receivers.push_back(i.second[j].to);
                    tris.cumulativeGasUsed.push_back(dev::u256(i.second[j].cumulativeGasUsed));
                    tris.gasUsed.push_back(dev::u256(i.second[j].gasUsed));
                    tris.contractAddresses.push_back(i.second[j].contractAddress);
                    tris.logs.push_back(logEntriesSerialization(i.second[j].logs));
                    tris.excepted.push_back(uint32_t(static_cast<int>(i.second[j].excepted)));
                    tris.exceptedMessage.push_back(i.second[j].exceptedMessage);
                    tris.outputIndexes.push_back(i.second[j].outputIndex);
                    tris.blooms.push_back(i.second[j].bloom);
                    tris.stateRoots.push_back(i.second[j].stateRoot);
                    tris.utxoRoots.push_back(i.second[j].utxoRoot);
                }

                dev::RLPStream streamRLP(16);
                streamRLP << tris.blockHashes << tris.blockNumbers << tris.transactionHashes << tris.transactionIndexes << tris.senders;
                streamRLP << tris.receivers << tris.cumulativeGasUsed << tris.gasUsed << tris.contractAddresses << tris.logs << tris.excepted << tris.exceptedMessage << tris.outputIndexes << tris.blooms << tris.stateRoots << tris.utxoRoots;

                dev::bytes data = streamRLP.out();
                std::string stringData(data.begin(), data.end());
                leveldb::Slice value(stringData);
                status = db->Put(leveldb::WriteOptions(), key, value);
                assert(status.ok());
            }
        }
        m_cache_result.clear();
    }
}

bool StorageResults::readResult(dev::h256 const& _key, std::vector<TransactionReceiptInfo>& _result){

    std::string value;
    std::string keyTemp = _key.hex();;
    leveldb::Slice key(keyTemp);
    leveldb::Status s = db->Get(leveldb::ReadOptions(), key, &value);

	if(!s.IsNotFound() && s.ok()){
        
        TransactionReceiptInfoSerialized tris;

		dev::RLP state(value);
        tris.blockHashes = state[0].toVector<dev::h256>();
		tris.blockNumbers = state[1].toVector<uint32_t>();
		tris.transactionHashes = state[2].toVector<dev::h256>();
        tris.transactionIndexes = state[3].toVector<uint32_t>();
        tris.senders = state[4].toVector<dev::h160>();
        tris.receivers = state[5].toVector<dev::h160>();
        tris.cumulativeGasUsed = state[6].toVector<dev::u256>();
        tris.gasUsed = state[7].toVector<dev::u256>();
        tris.contractAddresses = state[8].toVector<dev::h160>();
        tris.logs = state[9].toVector<logEntriesSerialize>();
        if(state.itemCount() >= 11)
            tris.excepted = state[10].toVector<uint32_t>();
        if(state.itemCount() >= 12)
            tris.exceptedMessage = state[11].toVector<std::string>();
        if(state.itemCount() >= 13)
            tris.outputIndexes = state[12].toVector<uint32_t>();
        if(state.itemCount() >= 14)
            tris.blooms = state[13].toVector<dev::h2048>();
        if(state.itemCount() >= 15)
            tris.stateRoots = state[14].toVector<dev::h256>();
        if(state.itemCount() >= 16)
            tris.utxoRoots = state[15].toVector<dev::h256>();

        for(size_t j = 0; j < tris.blockHashes.size(); j++){
            TransactionReceiptInfo tri{
                h256Touint(tris.blockHashes[j]),
                tris.blockNumbers[j],
                h256Touint(tris.transactionHashes[j]),
                tris.transactionIndexes[j],
                tris.senders[j],
                tris.receivers[j],
                uint64_t(tris.cumulativeGasUsed[j]),
                uint64_t(tris.gasUsed[j]),
                tris.contractAddresses[j],
                logEntriesDeserialize(tris.logs[j]),
                state.itemCount() >= 11 ? static_cast<dev::eth::TransactionException>(tris.excepted[j]) : dev::eth::TransactionException::NoInformation,
                state.itemCount() >= 12 ? tris.exceptedMessage[j] : "",
                state.itemCount() >= 13 ? tris.outputIndexes[j] : 0xffffffff,
                state.itemCount() >= 14 ? tris.blooms[j] : dev::h2048(),
                state.itemCount() >= 15 ? tris.stateRoots[j] : dev::h256(),
                state.itemCount() >= 16 ? tris.utxoRoots[j] : dev::h256()
            };
            _result.push_back(tri);
        }
		return true;
	}
	return false;
}

logEntriesSerialize StorageResults::logEntriesSerialization(dev::eth::LogEntries const& _logs){
	logEntriesSerialize result;
	for(dev::eth::LogEntry i : _logs){
		result.push_back(std::make_pair(i.address, std::make_pair(i.topics, i.data)));
	}
	return result;
}

dev::eth::LogEntries StorageResults::logEntriesDeserialize(logEntriesSerialize const& _logs){
	dev::eth::LogEntries result;
	for(std::pair<dev::Address, std::pair<dev::h256s, dev::bytes>> i : _logs){
		result.push_back(dev::eth::LogEntry(i.first, i.second.first, dev::bytes(i.second.second)));
	}
	return result;
}
