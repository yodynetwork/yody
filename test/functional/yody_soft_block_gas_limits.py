#!/usr/bin/env python3

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
from test_framework.script import *
from test_framework.p2p import *
from test_framework.address import *
from test_framework.yody import *
import sys
import random


class YodySoftMinerGasRelatedLimitsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 8
        self.extra_args = [
            ["-staker-soft-block-gas-limit=1000000000", '-rpcmaxgasprice=10000000', '-acceptnonstdtxn'],
            ["-staker-soft-block-gas-limit=400000", '-rpcmaxgasprice=10000000', '-acceptnonstdtxn'],
            ["-staker-soft-block-gas-limit=0", '-rpcmaxgasprice=10000000', '-acceptnonstdtxn'],
            ["-staker-max-tx-gas-limit=100000000", '-rpcmaxgasprice=10000000', '-acceptnonstdtxn'],
            ["-staker-max-tx-gas-limit=100000", '-rpcmaxgasprice=10000000', '-acceptnonstdtxn'],
            ["-staker-max-tx-gas-limit=0", '-rpcmaxgasprice=10000000', '-acceptnonstdtxn'],
            ["-staker-min-tx-gas-price=0", '-rpcmaxgasprice=10000000', '-acceptnonstdtxn'],
            ["-staker-min-tx-gas-price=0.0001", '-rpcmaxgasprice=10000000', '-acceptnonstdtxn']
        ]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    # Make sure that the hard block gas limit never differs in the evm.
    def verify_hard_block_gas_limit_test(self):
        """
        pragma solidity ^0.4.12;
        contract Test {
            uint public stateChanger;
            
            function () payable {
                stateChanger = block.gaslimit;
            }
        }
        """
        contract_bytecode = "60606040523415600e57600080fd5b5b60a08061001d6000396000f30060606040523615603d576000357c0100000000000000000000000000000000000000000000000000000000900463ffffffff1680636c7804b5146048575b5b456000819055505b005b3415605257600080fd5b6058606e565b6040518082815260200191505060405180910390f35b600054815600a165627a7a72305820c76a25c264d2d62cf880c85e7c616a1f21d93587aebe38ce85b8467d0cb7566f0029"
        contract_address = self.nodes[0].createcontract(contract_bytecode)['address']
        self.nodes[0].generate(1)
        self.sync_all()
        for node in self.nodes:
            node.sendtocontract(contract_address, "00", 1, 1000000, 0.0001)
            node.generate(1)
            self.sync_all()
        assert_equal(self.nodes[0].getrawmempool(), [])
        assert_equal(self.nodes[0].listcontracts()[contract_address], 8)


    def send_raw_to_contract(self, node, contract_address, gas_limit, gas_price, num_outputs=1):
        unspent = node.listunspent()[0]
        tx = CTransaction()
        tx.vin = [CTxIn(COutPoint(int(unspent['txid'], 16), unspent['vout']), nSequence=0)]
        amount = int((float(str(unspent['amount']))*COIN)) // num_outputs -  gas_price*gas_limit

        tx.vout = [CTxOut(amount, scriptPubKey=CScript([b"\x04", CScriptNum(gas_limit), CScriptNum(gas_price), b"\x00", hex_str_to_bytes(contract_address), OP_CALL])) for i in range(num_outputs)]
        tx_hex_signed = node.signrawtransactionwithwallet(bytes_to_hex_str(tx.serialize()))['hex']
        return node.sendrawtransaction(tx_hex_signed)

    def run_test(self):
        for i in range(len(self.nodes)):
            for j in range(i+1, len(self.nodes)):
                self.connect_nodes(i, j)
        for node in self.nodes:
            node.generate(10)
            self.sync_all()
        generatesynchronized(self.nodes[0], COINBASE_MATURITY, None, self.nodes)
        self.sync_all()
        """
        pragma solidity ^0.4.12;

        contract Test{
            function () payable { assert(false); }
        }
        """
        contract_address = self.nodes[0].createcontract("6080604052348015600f57600080fd5b50604280601d6000396000f3fe60806040526000600b57fe5b00fea265627a7a72305820db0c52d10779ca6ec85aa4cb3d532438cd960d532240f8f61b226bbdab137f4d64736f6c634300050a0032")['address']
        self.nodes[0].generate(1)
        assert(contract_address in self.nodes[0].listcontracts())
        self.sync_all()

        # Make sure that the soft block gas limit default to the hard block gas limit if the soft one exceeds the hard one.
        txid1 = self.send_raw_to_contract(self.nodes[0], contract_address, 40000000, 2000)
        txid2 = self.send_raw_to_contract(self.nodes[0], contract_address, 40000000, 1000)
        self.sync_all()
        for node_that_cannot_mine_the_txs in self.nodes[1:3]:
            block_hash = node_that_cannot_mine_the_txs.generate(1)[0]
            assert_equal(len(node_that_cannot_mine_the_txs.getblock(block_hash)['tx']), 1)
            assert(txid1 in node_that_cannot_mine_the_txs.getrawmempool())
            assert(txid2 in node_that_cannot_mine_the_txs.getrawmempool())
            self.sync_all()

        block_hash = self.nodes[0].generate(1)[0]
        # coinbase + txid1 + refund tx.
        assert_equal(len(self.nodes[0].getblock(block_hash)['tx']), 3)
        assert(txid2 in self.nodes[0].getrawmempool())
        block_hash = self.nodes[0].generate(1)[0]
        assert_equal(len(self.nodes[0].getblock(block_hash)['tx']), 3)
        assert_equal(self.nodes[0].getrawmempool(), [])
        self.sync_all()

        for i in range(5):
            self.send_raw_to_contract(self.nodes[1], contract_address, 100000, 2000)
        self.sync_all()

        block_hash = self.nodes[2].generate(1)[0]
        assert_equal(len(self.nodes[2].getblock(block_hash)['tx']), 1)
        assert_equal(len(self.nodes[2].getrawmempool()), 5)
        self.sync_all()

        block_hash = self.nodes[1].generate(1)[0]
        assert_equal(len(self.nodes[1].getblock(block_hash)['tx']), 9)
        assert_equal(len(self.nodes[1].getrawmempool()), 1)
        self.sync_all()

        block_hash = self.nodes[0].generate(1)[0]
        assert_equal(len(self.nodes[0].getblock(block_hash)['tx']), 3)
        assert_equal(len(self.nodes[0].getrawmempool()), 0)
        self.sync_all()

        # Test the soft gas limit
        self.send_raw_to_contract(self.nodes[4], contract_address, 100000, 2000)
        self.send_raw_to_contract(self.nodes[4], contract_address, 100001, 2000)
        self.sync_all()

        block_hash = self.nodes[5].generate(1)[0]
        assert_equal(len(self.nodes[5].getblock(block_hash)['tx']), 1)
        assert_equal(len(self.nodes[5].getrawmempool()), 2)
        self.sync_all()

        block_hash = self.nodes[4].generate(1)[0]
        assert_equal(len(self.nodes[4].getblock(block_hash)['tx']), 3)
        assert_equal(len(self.nodes[4].getrawmempool()), 1)
        self.sync_all()

        block_hash = self.nodes[4].generate(1)[0]
        assert_equal(len(self.nodes[4].getblock(block_hash)['tx']), 1)
        assert_equal(len(self.nodes[4].getrawmempool()), 1)
        self.sync_all()

        block_hash = self.nodes[3].generate(1)[0]
        assert_equal(len(self.nodes[3].getblock(block_hash)['tx']), 3)
        assert_equal(len(self.nodes[3].getrawmempool()), 0)


        self.send_raw_to_contract(self.nodes[7], contract_address, 100000, 9900)
        self.send_raw_to_contract(self.nodes[7], contract_address, 100000, 10000)
        self.sync_all()
        block_hash = self.nodes[7].generate(1)[0]
        assert_equal(len(self.nodes[7].getblock(block_hash)['tx']), 3)
        self.nodes[7].generate(1)
        assert_equal(len(self.nodes[7].getrawmempool()), 1)
        self.sync_all()
        self.nodes[6].generate(1)
        assert_equal(len(self.nodes[6].getrawmempool()), 0)
        self.sync_all()

        # Also make sure that the update counting the sum gas of all outputs works correctly for the soft tx gas limit
        self.send_raw_to_contract(self.nodes[4], contract_address, 34000, 1000, num_outputs=3)
        self.nodes[4].generate(1)
        assert_equal(len(self.nodes[4].getrawmempool()), 1)
        self.sync_all()
        assert_equal(len(self.nodes[3].getrawmempool()), 1)
        self.nodes[3].generate(1)
        self.sync_all()
        assert_equal(len(self.nodes[3].getrawmempool()), 0)
        assert_equal(len(self.nodes[4].getrawmempool()), 0)

        self.verify_hard_block_gas_limit_test()

if __name__ == '__main__':
    YodySoftMinerGasRelatedLimitsTest().main()
