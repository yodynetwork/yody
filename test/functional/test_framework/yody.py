from .address import *
from .script import *
from .p2p import *
from .util import *
from .yodyconfig import *
from .blocktools import *
from .key import *
from .segwit_addr import *
import io
import base64
import math
import pprint

pp = pprint.PrettyPrinter()

def generatesynchronized(node, numblocks, address=None, sync_with_nodes=[], mocktime=None):
    if not address:
        address = node.getnewaddress()
 
    startTime = time.time()
    blockhashes = []
    for i in range(0, max(numblocks//16, 0)):
        blockhashes += node.generatetoaddress(16, address)
        wait_until_helper(lambda: all(n.getbestblockhash() == node.getbestblockhash() for n in sync_with_nodes))

        # If more than 60 seconds elapses during the block generation, the nodes will disconnect since
        # the inactivity check for networking mix mocked and non-mocked time.

 
    if numblocks % 16:
        blockhashes += node.generatetoaddress(numblocks % 16, address)
        wait_until_helper(lambda: all(n.getbestblockhash() == node.getbestblockhash() for n in sync_with_nodes))
    return blockhashes

def generateinitial(node, numblocks, address=None, sync_with_nodes=[]):
    mocktime = node.getblock(node.getbestblockhash())['mocktime']
    for n in [node] + sync_with_nodes:
        n.setmocktime(mocktime)

    blockhashes = node.generatetoaddress(numblocks, address)

    for n in [node] + sync_with_nodes:
        n.setmocktime(0)

    return blockhashes

def make_transaction(node, vin, vout):
    tx = CTransaction()
    tx.vin = vin
    tx.vout = vout
    tx.rehash()

    unsigned_raw_tx = bytes_to_hex_str(tx.serialize_without_witness())
    signed_raw_tx = node.signrawtransactionwithwallet(unsigned_raw_tx)['hex']
    return signed_raw_tx

def make_vin(node, value):
    addr = node.getnewaddress()
    txid_hex = node.sendtoaddress(addr, value/COIN)
    txid = int(txid_hex, 16)
    node.generate(1)
    raw_tx = node.decoderawtransaction(node.gettransaction(txid_hex)['hex'])

    for vout_index, txout in enumerate(raw_tx['vout']):
        if txout['scriptPubKey']['address'] == addr:
            break
    else:
        assert False

    return CTxIn(COutPoint(txid, vout_index), nSequence=0)

def make_op_create_output(node, value, version, gas_limit, gas_price, data):
    scriptPubKey = CScript()
    scriptPubKey += version
    scriptPubKey += gas_limit
    scriptPubKey += gas_price
    scriptPubKey += data
    scriptPubKey += OP_CREATE
    return CTxOut(value, scriptPubKey)

def make_op_call_output(value, version, gas_limit, gas_price, data, contract):
    scriptPubKey = CScript()
    scriptPubKey += version
    scriptPubKey += gas_limit
    scriptPubKey += gas_price
    scriptPubKey += data
    scriptPubKey += contract
    scriptPubKey += OP_CALL
    return CTxOut(value, scriptPubKey)

def convert_btc_address_to_yody(addr, main=False):
    version, hsh, checksum = base58_to_byte(addr, 25)
    if version == 111:
        return keyhash_to_p2pkh(binascii.unhexlify(hsh), main)

    if version == 196:
        return scripthash_to_p2sh(binascii.unhexlify(hsh), main)
    assert(False)

def convert_btc_bech32_address_to_yody(addr, main=False, encoding=Encoding.BECH32):
    encoding, hdr, data = bech32_decode(addr)
    return bech32_encode(encoding, 'qcrt', data)


def p2pkh_to_hex_hash(address):
    return str(base58_to_byte(address, 25)[1])[2:-1]

def hex_hash_to_p2pkh(hex_hash):
    return keyhash_to_p2pkh(hex_str_to_bytes(hex_hash))

def assert_vin(tx, expected_vin):
    assert_equal(len(tx['vin']), len(expected_vin))
    matches = []
    for expected in expected_vin:
        for i in range(len(tx['vin'])):
            if i not in matches and expected[0] == tx['vin'][i]['scriptSig']['asm']:
                matches.append(i)
                break
    assert_equal(len(matches), len(expected_vin))

def assert_vout(tx, expected_vout):
    assert_equal(len(tx['vout']), len(expected_vout))
    matches = []
    for expected in expected_vout:
        for i in range(len(tx['vout'])):
            if i not in matches and expected[0] == tx['vout'][i]['value'] and expected[1] == tx['vout'][i]['scriptPubKey']['type']:
                matches.append(i)
                break
    assert_equal(len(matches), len(expected_vout))

def rpc_sign_transaction(node, tx):
    ret = node.signrawtransactionwithwallet(bytes_to_hex_str(tx.serialize()))
    if not ret['complete']:
        print(ret)
        assert(ret['complete'])
    tx_signed_raw_hex = ret['hex']
    f = io.BytesIO(hex_str_to_bytes(tx_signed_raw_hex))
    tx_signed = CTransaction()
    tx_signed.deserialize(f)
    return tx_signed

def make_vin_from_unspent(node, unspents=None, value=2000000000000, address=None):
    if not unspents:
        unspents = node.listunspent()
    for i in range(len(unspents)):
        unspent = unspents[i]
        if unspent['amount'] == value/COIN and (not address or address == unspent['address']):
            unspents.pop(i)
            return CTxIn(COutPoint(int(unspent['txid'], 16), unspent['vout']), nSequence=0)
    return None

def read_evm_array(node, address, abi, ignore_nulls=True):
    arr = []
    index = 0
    ret = node.callcontract(address, abi + hex(index)[2:].zfill(64))
    while ret['executionResult']['excepted'] == 'None':
        if int(ret['executionResult']['output'], 16) != 0 or not ignore_nulls:
            arr.append(ret['executionResult']['output'])
        index += 1
        ret = node.callcontract(address, abi + hex(index)[2:].zfill(64))
    return arr

class DGPState:
    def __init__(self, node, contract_address):
        self.last_state_assert_block_height = 0
        self.node = node
        self.contract_address = contract_address
        self.param_count = 0
        self.gov_keys = []
        self.admin_keys = []
        self.params_for_block = []
        self.param_address_at_indices = []
        self.required_votes = [0, 0, 0]
        self.current_on_vote_statuses = [
            [False, False, False],
            [False, False, False],
            [False, False]
        ]
        self.current_on_vote_address_proposals = [
            ["0", "0", "0"],
            ["0", "0"]
        ]
        self.abiAddAddressProposal = "bf5f1e83" #addAddressProposal(address,uint256)
        self.abiRemoveAddressProposal = "4cc0e2bc" # removeAddressProposal(address,uint256)
        self.abiChangeValueProposal = "19971cbd" # changeValueProposal(uint256,uint256)
        self.abiAlreadyVoted = "e9944a81" # alreadyVoted(address,address[])
        self.abiGetAddressesList = "850d9758" # getAddressesList(uint256)
        self.abiGetArrayNonNullLength = "9b216626" # getArrayNonNullLenght(address[])
        self.abiGetCurrentOnVoteAddressProposal = "0c83ebac" # getCurrentOnVoteAddressProposal(uint256,uint256)
        self.abiGetCurrentOnVoteStatus = "5f302e8b" # getCurrentOnVoteStatus(uint256,uint256)
        self.abiGetCurrentOnVoteValueProposal = "4364725c" # getCurrentOnVoteValueProposal(uint256)
        self.abiGetCurrentOnVoteVotes = "f9f51401" # getCurrentOnVoteVotes(uint256,uint256)
        self.abiGetParamAddressAtIndex = "15341747" # getParamAddressAtIndex(uint256)
        self.abiGetParamCount = "27e35746" # getParamCount()
        self.abiGetParamHeightAtIndex = "8a5a9d07" # getParamHeightAtIndex(uint256)
        self.abiGetParamsForBlock = "f769ac48" # getParamsForBlock(uint256)
        self.abiGetRequiredVotes = "1ec28e0f" # getRequiredVotes(uint256)
        self.abiIsAdminKey = "6b102c49" # isAdminKey(address)
        self.abiIsGovKey = "7b993bf3" # isGovKey(address)
        self.abiSetInitialAdmin = "6fb81cbb" # setInitialAdmin()
        self.abiTallyAdminVotes = "bec171e5" # tallyAdminVotes(address[])
        self.abiTallyGovVotes = "4afb4f11" # tallyGovVotes(address[])
        self.abiGovKeys = "30a79873" # govKeys(uint256)
        self.abiAdminKeys = "aff125f6" # adminKeys(uint256)

    def send_set_initial_admin(self, sender):
        self.node.sendtoaddress(sender, 1)
        self.node.sendtocontract(self.contract_address, self.abiSetInitialAdmin, 0, 2000000, YODY_MIN_GAS_PRICE_STR, sender)

    def send_add_address_proposal(self, proposal_address, type1, sender):
        self.node.sendtoaddress(sender, 1)
        txid = self.node.sendtocontract(self.contract_address, self.abiAddAddressProposal + proposal_address.zfill(64) + hex(type1)[2:].zfill(64), 0, 20000000, YODY_MIN_GAS_PRICE_STR, sender)['txid']
        return txid

    def send_remove_address_proposal(self, proposal_address, type1, sender):
        self.node.sendtoaddress(sender, 1)
        txid = self.node.sendtocontract(self.contract_address, self.abiRemoveAddressProposal + proposal_address.zfill(64) + hex(type1)[2:].zfill(64), 0, 20000000, YODY_MIN_GAS_PRICE_STR, sender)['txid']
        return txid

    def send_change_value_proposal(self, uint_proposal, type1, sender):
        self.node.sendtoaddress(sender, 1)
        txid = self.node.sendtocontract(self.contract_address, self.abiChangeValueProposal + hex(uint_proposal)[2:].zfill(64) + hex(type1)[2:].zfill(64), 0, 20000000, YODY_MIN_GAS_PRICE_STR, sender)['txid']
        return txid


    def assert_state(self):
        # This assertion is only to catch potential errors in the test code (if we forget to add a generate after an evm call)
        assert(self.last_state_assert_block_height < self.node.getblockcount())
        self.last_state_assert_block_height = self.node.getblockcount()

        self._assert_param_count(self.param_count)
        self._assert_gov_keys_equal(self.gov_keys)
        self._assert_admin_keys_equal(self.admin_keys)
        for block_height, param_for_block in self.params_for_block:
            self._assert_params_for_block(block_height, param_for_block)
        # Make sure that there are no subsequent params for blocks
        if self.params_for_block:
            ret = self.node.callcontract(self.contract_address, self.abiGetParamsForBlock + hex(0x2fff)[2:].zfill(64))
            assert_equal(int(ret['executionResult']['output'], 16), int(param_for_block, 16))
        else:
            ret = self.node.callcontract(self.contract_address, self.abiGetParamsForBlock + hex(0x2fff)[2:].zfill(64))
            assert_equal(int(ret['executionResult']['output'], 16), 0)


        for index, param_address_at_index in enumerate(self.param_address_at_indices):
            self._assert_param_address_at_index(index, param_address_at_index)
        # Make sure that there are no subsequent params at the next index
        if self.param_address_at_indices:
            ret = self.node.callcontract(self.contract_address, self.abiGetParamAddressAtIndex + hex(index+1)[2:].zfill(64))
            assert(ret['executionResult']['excepted'] != 'None')
        else:
            ret = self.node.callcontract(self.contract_address, self.abiGetParamAddressAtIndex + hex(0x0)[2:].zfill(64))
            assert(ret['executionResult']['excepted'] != 'None')


        for type1, required_votes in enumerate(self.required_votes):
            self._assert_required_votes(type1, required_votes)
        for type1, arr1 in enumerate(self.current_on_vote_statuses):
            for type2, current_on_vote_status in enumerate(arr1):
                self._assert_current_on_vote_status(type1, type2, current_on_vote_status)
        for type1, arr1 in enumerate(self.current_on_vote_address_proposals):
            for type2, current_on_vote_address_proposal in enumerate(arr1):
                self._assert_current_on_vote_address_proposal(type1, type2, current_on_vote_address_proposal)

    """
    function getRequiredVotes(uint _type) constant returns (uint val){
        // type 0: adminVotesForParams
        // type 1: govVotesForParams
        // type 2: adminVotesForManagement
        if(_type>2) throw; // invalid type
        if(_type==0)return activeVotesRequired.adminVotesForParams;
        if(_type==1)return activeVotesRequired.govVotesForParams;
        if(_type==2)return activeVotesRequired.adminVotesForManagement;
    }
    """
    def _assert_required_votes(self, type1, expected_required_votes):
        ret = self.node.callcontract(self.contract_address, self.abiGetRequiredVotes + str(type1).zfill(64))
        assert_equal(int(ret['executionResult']['output'], 16), expected_required_votes)

    """
   function getCurrentOnVoteStatus(uint _type, uint _type2) constant returns (bool val){
        // type 0: addAddress
        // type 1: changeValue
        // type 2: removeAddress

        // type2 0: adminKey
        // type2 1: govKey
        // type2 2: paramsAddress

        if(_type>2 || _type2>2) throw; // invalid type
        if(_type==0)return currentProposals.keys[_type2].onVote;
        if(_type==1)return currentProposals.uints[_type2].onVote;
        if(_type==2)return currentProposals.removeKeys[_type2].onVote;
    }
    """
    def _assert_current_on_vote_status(self, type1, type2, expected_current_on_vote_status):
        ret = self.node.callcontract(self.contract_address, self.abiGetCurrentOnVoteStatus + str(type1).zfill(64) + str(type2).zfill(64))
        assert_equal(int(ret['executionResult']['output'], 16), expected_current_on_vote_status)

    """
    function getCurrentOnVoteAddressProposal(uint _type, uint _type2) constant returns (address val){
        // type 0: addAddress
        // type 1: removeAddress

        // type2 0: adminKey
        // type2 1: govKey
        // type2 2: paramsAddress

        if(_type>1 || _type2>2) throw; // invalid type
        if(_type==0)return currentProposals.keys[_type2].proposal;
        if(_type==1)return currentProposals.removeKeys[_type2].proposal;
    }
    """
    def _assert_current_on_vote_address_proposal(self, type1, type2, expected_address):
        ret = self.node.callcontract(self.contract_address, self.abiGetCurrentOnVoteAddressProposal + hex(type1)[2:].zfill(64) + hex(type2)[2:].zfill(64))
        assert_equal(int(ret['executionResult']['output'], 16), int(expected_address, 16))

    """
    function getCurrentOnVoteValueProposal(uint _type) constant returns (uint val){
        // type 0: adminVotesForParams
        // type 1: govVotesForParams
        // type 2: adminVotesForManagement

        if(_type>2) throw; // invalid type
        return currentProposals.uints[_type].proposal;
    }
    """
    def _assert_current_on_vote_value_proposal(self, type1, expected_proposal):
        ret = self.node.callcontract(self.contract_address, self.abiGetCurrentOnVoteValueProposal + hex(type1)[2:].zfill(64))
        assert_equal(int(ret['executionResult']['output'], 16), expected_proposal)

    """
    function getParamsForBlock(uint _reqBlockHeight) constant returns (address paramsAddress){
        uint i;
        for(i=paramsHistory.length-1;i>0;i--){
            if(paramsHistory[i].blockHeight<=_reqBlockHeight)return paramsHistory[i].paramsAddress;
        }
        if(paramsHistory[0].blockHeight<=_reqBlockHeight)return paramsHistory[0].paramsAddress;
        return 0;
    }
    """
    def _assert_params_for_block(self, required_block_height, expected_param_address):
        ret = self.node.callcontract(self.contract_address, self.abiGetParamsForBlock + hex(required_block_height)[2:].zfill(64))
        assert_equal(int(ret['executionResult']['output'], 16), int(expected_param_address, 16))

    """
    function getParamAddressAtIndex(uint _paramIndex) constant returns (address paramsAddress){
        return paramsHistory[_paramIndex].paramsAddress;
    }
    """
    def _assert_param_address_at_index(self, param_index, expected_param_address):
        ret = self.node.callcontract(self.contract_address, self.abiGetParamAddressAtIndex + hex(param_index)[2:].zfill(64))
        assert_equal(int(ret['executionResult']['output'], 16), int(expected_param_address, 16))


    """
    function getParamHeightAtIndex(uint _paramIndex) constant returns (uint paramsHeight){
        return paramsHistory[_paramIndex].blockHeight;
    }
    """
    def _assert_param_block_height_at_index(self, param_index, expected_block_height):
        ret = self.node.callcontract(self.contract_address, self.abiGetParamHeightAtIndex + hex(param_index)[2:].zfill(64))
        assert_equal(int(ret['executionResult']['output'], 16), expected_block_height)

    """
    function getParamCount() constant returns (uint paramsCount){
        return paramsHistory.length;
    }
    """
    def _assert_param_count(self, expected_param_count):
        ret = self.node.callcontract(self.contract_address, self.abiGetParamCount)
        assert_equal(int(ret['executionResult']['output'], 16), expected_param_count)

    def _assert_gov_keys_equal(self, expected_gov_keys):
        real_gov_keys = read_evm_array(self.node, self.contract_address, self.abiGovKeys)
        assert_equal(len(real_gov_keys), len(expected_gov_keys))
        for real, expected in zip(real_gov_keys, expected_gov_keys):
            assert_equal(int(real, 16), int(expected, 16))

    def _assert_admin_keys_equal(self, expected_admin_keys):
        real_admin_keys = read_evm_array(self.node, self.contract_address, self.abiAdminKeys)
        assert_equal(len(real_admin_keys), len(expected_admin_keys))
        for real, expected in zip(real_admin_keys, expected_admin_keys):
            assert_equal(int(real, 16), int(expected, 16))


def collect_prevouts(node, amount=None, address=None, min_confirmations=COINBASE_MATURITY, min_amount=0):
    blocks = []
    for block_no in range(1, node.getblockcount()+1):
        blocks.append(node.getblock(node.getblockhash(block_no)))


    staking_prevouts = []
    for unspent in node.listunspent():
        for block in blocks:
            if unspent['txid'] in block['tx']:
                tx_block_time = block['time']
                break
        else:
            assert(False)
        if unspent['confirmations'] > min_confirmations and (not amount or amount == unspent['amount']) and (not address or address == unspent['address']) and unspent['amount'] >= min_amount:
            staking_prevouts.append((COutPoint(int(unspent['txid'], 16), unspent['vout']), int(unspent['amount']*COIN), tx_block_time))
    return staking_prevouts


def create_unsigned_pos_block(node, staking_prevouts, nTime=None):
    tip = node.getblock(node.getbestblockhash())
    if not nTime:
        current_time = int(time.time()) + TIMESTAMP_MASK+1
        nTime = current_time & (0xffffffff - TIMESTAMP_MASK)

    parent_block_stake_modifier = int(tip['modifier'], 16)
    coinbase = create_coinbase(tip['height']+1)
    coinbase.vout[0].nValue = 0
    coinbase.vout[0].scriptPubKey = b""
    coinbase.rehash()
    block = create_block(int(tip['hash'], 16), coinbase, nTime)
    block.hashStateRoot = int(tip['hashStateRoot'], 16)
    block.hashUTXORoot = int(tip['hashUTXORoot'], 16)

    if not block.solve_stake(parent_block_stake_modifier, staking_prevouts):
        return None

    txout = node.gettxout(hex(block.prevoutStake.hash)[2:].zfill(64), block.prevoutStake.n)
    # input value + block reward
    out_value = int((float(str(txout['value'])) + INITIAL_BLOCK_REWARD_POS) * COIN) // 2

    # create a new private key used for block signing.
    block_sig_key = ECKey()
    block_sig_key.set(hash256(struct.pack('<I', 0)), False)
    pubkey = block_sig_key.get_pubkey().get_bytes()
    scriptPubKey = CScript([pubkey, OP_CHECKSIG])
    stake_tx_unsigned = CTransaction()

    stake_tx_unsigned.vin.append(CTxIn(block.prevoutStake))
    stake_tx_unsigned.vout.append(CTxOut())

    # Split the output value into two separate txs
    stake_tx_unsigned.vout.append(CTxOut(int(out_value), scriptPubKey))
    stake_tx_unsigned.vout.append(CTxOut(int(out_value), scriptPubKey))

    stake_tx_signed_raw_hex = node.signrawtransactionwithwallet(bytes_to_hex_str(stake_tx_unsigned.serialize()))['hex']
    f = io.BytesIO(hex_str_to_bytes(stake_tx_signed_raw_hex))
    stake_tx_signed = CTransaction()
    stake_tx_signed.deserialize(f)
    block.vtx.append(stake_tx_signed)
    block.hashMerkleRoot = block.calc_merkle_root()
    return (block, block_sig_key)


def create_unsigned_mpos_block(node, staking_prevouts, nTime=None, block_fees=0):
    mpos_block, block_sig_key = create_unsigned_pos_block(node, staking_prevouts, nTime)
    tip = node.getblock(node.getbestblockhash())

    # The block reward is constant for regtest
    stake_per_participant = int(INITIAL_BLOCK_REWARD_POS*COIN+block_fees) // MPOS_PARTICIPANTS

    for i in range(MPOS_PARTICIPANTS-1):
        partipant_block = node.getblock(node.getblockhash(tip['height']-500-i))
        participant_tx = node.decoderawtransaction(node.gettransaction(partipant_block['tx'][1])['hex'])
        participant_pubkey = hex_str_to_bytes(participant_tx['vout'][1]['scriptPubKey']['asm'].split(' ')[0])
        mpos_block.vtx[1].vout.append(CTxOut(stake_per_participant, CScript([OP_DUP, OP_HASH160, hash160(participant_pubkey), OP_EQUALVERIFY, OP_CHECKSIG])))

    # the input value
    txout = node.gettxout(hex(mpos_block.prevoutStake.hash)[2:], mpos_block.prevoutStake.n)

    # Reward per output
    main_staker_reward = (int(float(str(txout['value']))*COIN) + stake_per_participant)

    mpos_block.vtx[1].vout[1].nValue = main_staker_reward // 2
    mpos_block.vtx[1].vout[2].nValue = main_staker_reward // 2

    stake_tx_signed_raw_hex = node.signrawtransactionwithwallet(bytes_to_hex_str(mpos_block.vtx[1].serialize()))['hex']
    f = io.BytesIO(hex_str_to_bytes(stake_tx_signed_raw_hex))
    stake_tx_signed = CTransaction()
    stake_tx_signed.deserialize(f)
    mpos_block.vtx[1] = stake_tx_signed
    mpos_block.hashMerkleRoot = mpos_block.calc_merkle_root()
    return mpos_block, block_sig_key

# Generates 4490 - blockheight PoW blocks + 510 PoS blocks,
# i.e. block height afterwards will be 5000 and we will have valid MPoS participants.
def activate_mpos(node, use_cache=True):
    if not node.getblockcount():
        node.setmocktime(int(time.time()) - 1000000)
    node.generatetoaddress(4990-COINBASE_MATURITY-node.getblockcount(), "qSrM9K6FMhZ29Vkp8Rdk8Jp66bbfpjFETq")
    staking_prevouts = collect_prevouts(node, address="qSrM9K6FMhZ29Vkp8Rdk8Jp66bbfpjFETq")

    for i in range(COINBASE_MATURITY+10):
        time.sleep(0.05)
        nTime = (node.getblock(node.getbestblockhash())['time']+45) & 0xfffffff0
        node.setmocktime(nTime)
        block, block_sig_key = create_unsigned_pos_block(node, staking_prevouts, nTime=nTime)
        block.sign_block(block_sig_key)
        block.rehash()
        block_count = node.getblockcount()
        assert_equal(node.submitblock(bytes_to_hex_str(block.serialize())), None)
        assert_equal(node.getblockcount(), block_count+1)

        # Remove the staking prevout so we don't accidently reuse it
        for j in range(len(staking_prevouts)):
            prevout = staking_prevouts[j]
            if prevout[0].serialize() == block.prevoutStake.serialize():
                staking_prevouts.pop(j)
                break

        if len(staking_prevouts) < 20:
            staking_prevouts = collect_prevouts(node, address="qSrM9K6FMhZ29Vkp8Rdk8Jp66bbfpjFETq")


def wif_to_ECKey(wif):
    _, privkey, _ = base58_to_byte(wif, 38)
    bytes_privkey = hex_str_to_bytes(str(privkey)[2:-1])
    key = ECKey()
    # Assume always compressed, ignore last byte which specifies compression
    key.set(bytes_privkey[:-1], True)
    return key

def create_POD(delegator, delegator_address, staker_address):
    hex_hash = p2pkh_to_hex_hash(staker_address)
    b64_signature = delegator.signmessage(delegator_address, hex_hash)
    bytes_signature = base64.b64decode(b64_signature)
    return bytes_signature

def assert_delegation_reverted_with_message(delegator, abi, sender, message, gas=2250000):
    txid = delegator.sendtocontract(DELEGATION_CONTRACT_ADDRESS, abi, 0, gas, 0.00000040, sender)['txid']
    delegator.generate(1)
    receipt = delegator.gettransactionreceipt(txid)[0]
    assert_equal(receipt['excepted'], 'Revert')
    assert_equal(receipt['exceptedMessage'], message)
    #print("[+] passed " + message)

def assert_delegation_events_emitted(delegator, abi, sender, events=[], delegations={}, gas=2250000, expected_gas_consumed=0):
    txid = delegator.sendtocontract(DELEGATION_CONTRACT_ADDRESS, abi, 0, gas, 0.00000040, sender)['txid']
    delegator.generate(1)
    receipt = delegator.gettransactionreceipt(txid)[0]

    # Verify the fields of the log are as expected
    for ret, expected in zip(receipt['log'], events):
        for ret_indexed, expected_indexed in zip(ret['topics'], expected['topics']):
            assert_equal(ret_indexed, expected_indexed)
        assert_equal(ret['data'], expected['data'])

    # Check the state of the delegation attribute
    for delegate, delegate_data in delegations.items():
        out = delegator.callcontract(DELEGATION_CONTRACT_ADDRESS, "bffe3486" + delegate.zfill(64))['executionResult']['output']
        assert_equal(out[:64], delegate_data['staker'].zfill(64))
        assert_equal(out[64:128], delegate_data['fee'].zfill(64))
        assert_equal(out[128:192], delegate_data['blockHeight'].zfill(64))
        assert_equal(out[192:], "80".zfill(64) + hex(65 if delegate_data['pod'] else 0)[2:].zfill(64) + delegate_data['pod'])

    # Make sure we consume the minimum gas expected
    assert(receipt['gasUsed'] > expected_gas_consumed)

def get_delegate_abi(staker_address, fee, pod):
    padded_hex_pod = bytes_to_hex_str(pod) + "00"*31
    fee_hex = hex(fee)[2:]
    staker_address_hex = p2pkh_to_hex_hash(staker_address)
    abi = "4c0e968c"
    abi += staker_address_hex.zfill(64)
    abi += fee_hex.zfill(64)
    abi += "60".zfill(64)
    abi += hex(65)[2:].zfill(64)
    abi += padded_hex_pod
    return abi

def delegate_to_staker(delegator, delegator_address, staker_address, fee, pod):
    padded_hex_pod = bytes_to_hex_str(pod) + "00"*31
    fee_hex = hex(fee)[2:]
    expected_block_height = hex(delegator.getblockcount()+1)[2:]
    staker_address_hex = p2pkh_to_hex_hash(staker_address)
    delegator_address_hex = p2pkh_to_hex_hash(delegator_address)
    abi = get_delegate_abi(staker_address, fee, pod)
    assert_delegation_events_emitted(delegator, abi, delegator_address, events=[{
        "topics": [
            'a23803f3b2b56e71f2921c22b23c32ef596a439dbe03f7250e6b58a30eb910b5', # keccak256 of AddDelegation(...)
            staker_address_hex.zfill(64), # staker
            delegator_address_hex.zfill(64) # delegate
        ],
        # fee + block.number + offsetofpod + sizeofpod + pod
        "data": fee_hex.zfill(64) + expected_block_height.zfill(64) + "60".zfill(64) + hex(65)[2:].zfill(64) + padded_hex_pod
    }], delegations={
        delegator_address_hex: {
            "fee": fee_hex,
            "staker": staker_address_hex,
            "blockHeight": expected_block_height,
            "pod": padded_hex_pod
        }
    }, expected_gas_consumed=2000000)


def create_delegated_pos_block(staker, staker_eckey, staker_prevout, delegator_address_hex, pod, staking_fee_percentage, delegator_prevouts, nFees=0, nTime=None, use_pos_reward=False):
    tmp = create_unsigned_pos_block(staker, delegator_prevouts, nTime=nTime)
    if not tmp:
        return None

    block_subsidy = INITIAL_BLOCK_REWARD_POS if use_pos_reward else INITIAL_BLOCK_REWARD

    block, k = tmp
    # change the vin from the staker input to the delegator input
    staker_nas_txout = staker.gettxout(hex(staker_prevout.hash)[2:].zfill(64), staker_prevout.n)
    staker_nas_input_value = int(float(str(staker_nas_txout['value']))*COIN)

    block.vtx[1].vin[0] = CTxIn(staker_prevout)
    block.vtx[1].vout[1].scriptPubKey = CScript([staker_eckey.get_pubkey().get_bytes(), OP_CHECKSIG])
    block.vtx[1].vout[1].nValue = int(((block_subsidy*COIN+nFees) * staking_fee_percentage) // 100)
    block.vtx[1].vout[2].scriptPubKey = CScript([OP_DUP, OP_HASH160, hex_str_to_bytes(delegator_address_hex), OP_EQUALVERIFY, OP_CHECKSIG])
    block.vtx[1].vout[2].nValue = int(block_subsidy*COIN+nFees) - block.vtx[1].vout[1].nValue # subtract the staker's reward to get the delegator's reward (the delegator will ceil)
    block.vtx[1].vout[1].nValue += staker_nas_input_value # add the input value for the staker
    block.vtx[1] = rpc_sign_transaction(staker, block.vtx[1])
    block.vtx[1].rehash()
    block.hashMerkleRoot = block.calc_merkle_root()
    block.rehash()
    block.sign_block(staker_eckey, pod=pod)
    block.vchBlockSig = block.vchBlockSig + pod
    block.rehash()
    return block


