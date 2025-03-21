#!/usr/bin/env python3

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
from test_framework.script import *
from test_framework.p2p import *
from test_framework.yody import *
from test_framework.yodyconfig import *
from test_framework.blocktools import *
import time

NUM_OUTPUTS = 1000

class YodyGasLimit(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [["-staking=1"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.node = self.nodes[0]
        self.node.generate(100+COINBASE_MATURITY)
        tx = CTransaction()

        """
            contract Test {
                function() {
                    while(msg.gas > 0) {}
                }
            }
        """
        contract_address = self.node.createcontract("60606040523415600e57600080fd5b5b605080601c6000396000f30060606040525b3415600f57600080fd5b60225b5b60005a1115601f576013565b5b565b0000a165627a7a72305820efcd4d663aac9e7a94b44502e712d9eb63cd640efe3aebf9e79210ab63ea6ff60029")['address']
        self.node.generate(1)

        # Create a tx with 2000 outputs each with a gas stipend of 5*10^8 calling the contract.
        tx = CTransaction()
        tx.vin = [make_vin(self.node, NUM_OUTPUTS*5*COIN)]
        tx.vout = [CTxOut(0, CScript([b"\x04", int(5*COIN), YODY_MIN_GAS_PRICE, b"\x00", bytes.fromhex(contract_address), OP_CALL])) for i in range(NUM_OUTPUTS)]
        tx.rehash()
        signed_tx_hex = self.node.signrawtransactionwithwallet(bytes_to_hex_str(tx.serialize()))['hex']

        # We may want to reject transactions which exceed the gas limit outright.
        try: 
            self.node.sendrawtransaction(signed_tx_hex)
        except:
            pass

        print("Tx size", len(signed_tx_hex))
        t = time.time()
        self.node.generate(1)
        execution_time = time.time() - t
        print('execution time:', execution_time, 'seconds')
        assert(execution_time < 60)


if __name__ == '__main__':
    YodyGasLimit().main()
