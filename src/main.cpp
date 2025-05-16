// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "main.h"

#include "addrman.h"
#include "alert.h"
#include "base58.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "init.h"
#include "net.h"
#include "richlistdb.h"
#include "servicelistdb.h"
#include "serviceitemlistdb.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "rpcserver.h"
#include "wallet.h"

#include "util.h"

#include <sstream>

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/algorithm/string.hpp>

using namespace std;
using namespace boost;

#if defined(NDEBUG)
# error "Smileycoin cannot be compiled without assertions."
#endif

//
// Global state
//

static int64_t nTargetTimespan;
static const int64_t nTargetSpacing = 3 * 60; // 3 minutes for smileycoin
static int64_t nInterval;

//MultiAlgo Target updates
static const int64_t multiAlgoNum = 5; // Amount of algos
static int64_t multiAlgoTimespan; // Time per block per algo
static int64_t multiAlgoTargetSpacing; // NUM_ALGOS * 180 seconds = 900 seconds

static const int64_t nAveragingInterval = 60; // 60 blocks
static int64_t nAveragingTargetTimespan; // 10* NUM_ALGOS * 180

static const int64_t nMaxAdjustDown = 20; // 20% adjustment down
static const int64_t nMaxAdjustUp = 20; // 20% adjustment up
static const int64_t nLocalTargetAdjustment = 4; //target adjustment per algo

static const int64_t nAveragingIntervalV2 = 2; // 2 blocks
static const int64_t nMaxAdjustDownV2 = 8; // 8% adjustment down
static const int64_t nMaxAdjustUpV2 = 4; // 4% adjustment up
static const int64_t nLocalTargetAdjustmentV2 = 4; //target adjustment per algo

static int64_t nMinActualTimespan;
static int64_t nMaxActualTimespan;


CCriticalSection cs_main;

CTxMemPool mempool;

map<uint256, CBlockIndex*> mapBlockIndex;
CChain chainActive;
CChain chainMostWork;
int64_t nTimeBestReceived = 0;
int nScriptCheckThreads = 0;
bool fImporting = false;
bool fReindex = false;
bool fBenchmark = false;
bool fTxIndex = false;
unsigned int nCoinCacheSize = 5000;
uint256 hashGenesisBlock("0x660f734cf6c6d16111bde201bbd2122873f2f2c078b969779b9d4c99732354fd");

/** Fees smaller than this (in satoshi) are considered zero fee (for transaction creation) */
int64_t CTransaction::nMinTxFee = 100000;  // Override with -mintxfee

/** Fees smaller than this (in satoshi) are considered zero fee (for relaying and mining) */
int64_t CTransaction::nMinRelayTxFee = 100000;

static CMedianFilter<int> cPeerBlockCounts(8, 0); // Amount of blocks that other nodes claim to have

struct COrphanBlock {
	uint256 hashBlock;
	uint256 hashPrev;
	vector<unsigned char> vchBlock;
};
map<uint256, COrphanBlock*> mapOrphanBlocks;
multimap<uint256, COrphanBlock*> mapOrphanBlocksByPrev;

map<uint256, CTransaction> mapOrphanTransactions;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "Smileycoin Signed Message:\n";

// Settings
int miningAlgo = ALGO_SCRYPT;

// Internal stuff
namespace {
struct CBlockIndexWorkComparator
{
	bool operator()(CBlockIndex *pa, CBlockIndex *pb) {
		// First sort by most total work, ...
		if (pa->nChainWork > pb->nChainWork) return false;
		if (pa->nChainWork < pb->nChainWork) return true;

		// ... then by earliest time received, ...
		if (pa->nSequenceId < pb->nSequenceId) return false;
		if (pa->nSequenceId > pb->nSequenceId) return true;

		// Use pointer address as tie breaker (should only happen with blocks
		// loaded from disk, as those all have id 0).
		if (pa < pb) return false;
		if (pa > pb) return true;

		// Identical blocks.
		return false;
	}
};

CBlockIndex *pindexBestInvalid;
// may contain all CBlockIndex*'s that have validness >=BLOCK_VALID_TRANSACTIONS, and must contain those who aren't failed
set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexValid;

CCriticalSection cs_LastBlockFile;
CBlockFileInfo infoLastBlockFile;
int nLastBlockFile = 0;

// Every received block is assigned a unique and increasing identifier, so we
// know which one to give priority in case of a fork.
CCriticalSection cs_nBlockSequenceId;
// Blocks loaded from disk are assigned id 0, so start the counter at 1.
uint32_t nBlockSequenceId = 1;

// Sources of received blocks, to be able to send them reject messages or ban
// them, if processing happens afterwards. Protected by cs_main.
map<uint256, NodeId> mapBlockSource;

// Blocks that are in flight, and that are in the queue to be downloaded.
// Protected by cs_main.
struct QueuedBlock {
	uint256 hash;
	int64_t nTime;  // Time of "getdata" request in microseconds.
	int nQueuedBefore;  // Number of blocks in flight at the time of request.
};
map<uint256, pair<NodeId, list<QueuedBlock>::iterator> > mapBlocksInFlight;
map<uint256, pair<NodeId, list<uint256>::iterator> > mapBlocksToDownload;
}

//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

// These functions dispatch to one or all registered wallets

namespace {
struct CMainSignals {
	// Notifies listeners of updated transaction data (passing hash, transaction, and optionally the block it is found in.
	boost::signals2::signal<void (const uint256 &, const CTransaction &, const CBlock *)> SyncTransaction;
	// Notifies listeners of an erased transaction (currently disabled, requires transaction replacement).
	boost::signals2::signal<void (const uint256 &)> EraseTransaction;
	// Notifies listeners of an updated transaction without new data (for now: a coinbase potentially becoming visible).
	boost::signals2::signal<void (const uint256 &)> UpdatedTransaction;
	// Notifies listeners of a new active block chain.
	boost::signals2::signal<void (const CBlockLocator &)> SetBestChain;
	// Notifies listeners about an inventory item being seen on the network.
	boost::signals2::signal<void (const uint256 &)> Inventory;
	// Tells listeners to broadcast their data.
	boost::signals2::signal<void ()> Broadcast;
} g_signals;
}

void RegisterWallet(CWalletInterface* pwalletIn) {
	g_signals.SyncTransaction.connect(boost::bind(&CWalletInterface::SyncTransaction, pwalletIn, _1, _2, _3));
	g_signals.EraseTransaction.connect(boost::bind(&CWalletInterface::EraseFromWallet, pwalletIn, _1));
	g_signals.UpdatedTransaction.connect(boost::bind(&CWalletInterface::UpdatedTransaction, pwalletIn, _1));
	g_signals.SetBestChain.connect(boost::bind(&CWalletInterface::SetBestChain, pwalletIn, _1));
	g_signals.Inventory.connect(boost::bind(&CWalletInterface::Inventory, pwalletIn, _1));
	g_signals.Broadcast.connect(boost::bind(&CWalletInterface::ResendWalletTransactions, pwalletIn));
}

void UnregisterWallet(CWalletInterface* pwalletIn) {
	g_signals.Broadcast.disconnect(boost::bind(&CWalletInterface::ResendWalletTransactions, pwalletIn));
	g_signals.Inventory.disconnect(boost::bind(&CWalletInterface::Inventory, pwalletIn, _1));
	g_signals.SetBestChain.disconnect(boost::bind(&CWalletInterface::SetBestChain, pwalletIn, _1));
	g_signals.UpdatedTransaction.disconnect(boost::bind(&CWalletInterface::UpdatedTransaction, pwalletIn, _1));
	g_signals.EraseTransaction.disconnect(boost::bind(&CWalletInterface::EraseFromWallet, pwalletIn, _1));
	g_signals.SyncTransaction.disconnect(boost::bind(&CWalletInterface::SyncTransaction, pwalletIn, _1, _2, _3));
}

void UnregisterAllWallets() {
	g_signals.Broadcast.disconnect_all_slots();
	g_signals.Inventory.disconnect_all_slots();
	g_signals.SetBestChain.disconnect_all_slots();
	g_signals.UpdatedTransaction.disconnect_all_slots();
	g_signals.EraseTransaction.disconnect_all_slots();
	g_signals.SyncTransaction.disconnect_all_slots();
}

void SyncWithWallets(const uint256 &hash, const CTransaction &tx, const CBlock *pblock) {
	g_signals.SyncTransaction(hash, tx, pblock);
}

//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

namespace {

struct CBlockReject {
	unsigned char chRejectCode;
	string strRejectReason;
	uint256 hashBlock;
};

// Maintain validation-specific state about nodes, protected by cs_main, instead
// by CNode's own locks. This simplifies asynchronous operation, where
// processing of incoming data is done after the ProcessMessage call returns,
// and we're no longer holding the node's locks.
struct CNodeState {
	// Accumulated misbehaviour score for this peer.
	int nMisbehavior;
	// Whether this peer should be disconnected and banned.
	bool fShouldBan;
	// String name of this peer (debugging/logging purposes).
	std::string name;
	// List of asynchronously-determined block rejections to notify this peer about.
	std::vector<CBlockReject> rejects;
	list<QueuedBlock> vBlocksInFlight;
	int nBlocksInFlight;
	list<uint256> vBlocksToDownload;
	int nBlocksToDownload;
	int64_t nLastBlockReceive;
	int64_t nLastBlockProcess;

	CNodeState() {
		nMisbehavior = 0;
		fShouldBan = false;
		nBlocksToDownload = 0;
		nBlocksInFlight = 0;
		nLastBlockReceive = 0;
		nLastBlockProcess = 0;
	}
};

// Map maintaining per-node state. Requires cs_main.
map<NodeId, CNodeState> mapNodeState;

// Requires cs_main.
CNodeState *State(NodeId pnode) {
	map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
	if (it == mapNodeState.end())
		return NULL;
	return &it->second;
}

int GetHeight()
{
	LOCK(cs_main);
	return chainActive.Height();
}

void InitializeNode(NodeId nodeid, const CNode *pnode) {
	LOCK(cs_main);
	CNodeState &state = mapNodeState.insert(std::make_pair(nodeid, CNodeState())).first->second;
	state.name = pnode->addrName;
}

void FinalizeNode(NodeId nodeid) {
	LOCK(cs_main);
	CNodeState *state = State(nodeid);

	BOOST_FOREACH(const QueuedBlock& entry, state->vBlocksInFlight)
	mapBlocksInFlight.erase(entry.hash);
	BOOST_FOREACH(const uint256& hash, state->vBlocksToDownload)
	mapBlocksToDownload.erase(hash);

	mapNodeState.erase(nodeid);
}

// Requires cs_main.
void MarkBlockAsReceived(const uint256 &hash, NodeId nodeFrom = -1) {
	map<uint256, pair<NodeId, list<uint256>::iterator> >::iterator itToDownload = mapBlocksToDownload.find(hash);
	if (itToDownload != mapBlocksToDownload.end()) {
		CNodeState *state = State(itToDownload->second.first);
		state->vBlocksToDownload.erase(itToDownload->second.second);
		state->nBlocksToDownload--;
		mapBlocksToDownload.erase(itToDownload);
	}

	map<uint256, pair<NodeId, list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
	if (itInFlight != mapBlocksInFlight.end()) {
		CNodeState *state = State(itInFlight->second.first);
		state->vBlocksInFlight.erase(itInFlight->second.second);
		state->nBlocksInFlight--;
		if (itInFlight->second.first == nodeFrom)
			state->nLastBlockReceive = GetTimeMicros();
		mapBlocksInFlight.erase(itInFlight);
	}

}

// Requires cs_main.
bool AddBlockToQueue(NodeId nodeid, const uint256 &hash) {
	if (mapBlocksToDownload.count(hash) || mapBlocksInFlight.count(hash))
		return false;

	CNodeState *state = State(nodeid);
	if (state == NULL)
		return false;

	list<uint256>::iterator it = state->vBlocksToDownload.insert(state->vBlocksToDownload.end(), hash);
	state->nBlocksToDownload++;
	if (state->nBlocksToDownload > 5000)
		Misbehaving(nodeid, 10);
	mapBlocksToDownload[hash] = std::make_pair(nodeid, it);
	return true;
}

// Requires cs_main.
void MarkBlockAsInFlight(NodeId nodeid, const uint256 &hash) {
	CNodeState *state = State(nodeid);
	assert(state != NULL);

	// Make sure it's not listed somewhere already.
	MarkBlockAsReceived(hash);

	QueuedBlock newentry = {hash, GetTimeMicros(), state->nBlocksInFlight};
	if (state->nBlocksInFlight == 0)
		state->nLastBlockReceive = newentry.nTime; // Reset when a first request is sent.
	list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(), newentry);
	state->nBlocksInFlight++;
	mapBlocksInFlight[hash] = std::make_pair(nodeid, it);
}

}

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats) {
	LOCK(cs_main);
	CNodeState *state = State(nodeid);
	if (state == NULL)
		return false;
	stats.nMisbehavior = state->nMisbehavior;
	return true;
}

void RegisterNodeSignals(CNodeSignals& nodeSignals)
{
	nodeSignals.GetHeight.connect(&GetHeight);
	nodeSignals.ProcessMessages.connect(&ProcessMessages);
	nodeSignals.SendMessages.connect(&SendMessages);
	nodeSignals.InitializeNode.connect(&InitializeNode);
	nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
	nodeSignals.GetHeight.disconnect(&GetHeight);
	nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
	nodeSignals.SendMessages.disconnect(&SendMessages);
	nodeSignals.InitializeNode.disconnect(&InitializeNode);
	nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}

//////////////////////////////////////////////////////////////////////////////
//
// CChain implementation
//

CBlockIndex *CChain::SetTip(CBlockIndex *pindex) {
	if (pindex == NULL) {
		vChain.clear();
		return NULL;
	}
	vChain.resize(pindex->nHeight + 1);
	while (pindex && vChain[pindex->nHeight] != pindex) {
		vChain[pindex->nHeight] = pindex;
		pindex = pindex->pprev;
	}
	return pindex;
}

CBlockLocator CChain::GetLocator(const CBlockIndex *pindex) const {
	int nStep = 1;
	std::vector<uint256> vHave;
	vHave.reserve(32);

	if (!pindex)
		pindex = Tip();
	while (pindex) {
		vHave.push_back(pindex->GetBlockHash());
		// Stop when we have added the genesis block.
		if (pindex->nHeight == 0)
			break;
		// Exponentially larger steps back, plus the genesis block.
		int nHeight = std::max(pindex->nHeight - nStep, 0);
		// In case pindex is not in this chain, iterate pindex->pprev to find blocks.
		while (pindex->nHeight > nHeight && !Contains(pindex))
			pindex = pindex->pprev;
		// If pindex is in this chain, use direct height-based access.
		if (pindex->nHeight > nHeight)
			pindex = (*this)[nHeight];
		if (vHave.size() > 10)
			nStep *= 2;
	}

	return CBlockLocator(vHave);
}

CBlockIndex *CChain::FindFork(const CBlockLocator &locator) const {
	// Find the first block the caller has in the main chain
	BOOST_FOREACH(const uint256& hash, locator.vHave) {
		std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
		if (mi != mapBlockIndex.end())
		{
			CBlockIndex* pindex = (*mi).second;
			if (Contains(pindex))
				return pindex;
		}
	}
	return Genesis();
}

CCoinsViewCache *pcoinsTip = NULL;
CBlockTreeDB *pblocktree = NULL;

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx)
{
	uint256 hash = tx.GetHash();
	if (mapOrphanTransactions.count(hash))
		return false;

	// Ignore big transactions, to avoid a
	// send-big-orphans memory exhaustion attack. If a peer has a legitimate
	// large transaction with a missing parent then we assume
	// it will rebroadcast it later, after the parent transaction(s)
	// have been mined or received.
	// 10,000 orphans, each of which is at most 5,000 bytes big is
	// at most 500 megabytes of orphans:
	unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
	if (sz > 5000)
	{
		LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString());
		return false;
	}

	mapOrphanTransactions[hash] = tx;
	BOOST_FOREACH(const CTxIn& txin, tx.vin)
	mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

	LogPrint("mempool", "stored orphan tx %s (mapsz %u)\n", hash.ToString(),mapOrphanTransactions.size());

	return true;
}

void static EraseOrphanTx(uint256 hash)
		{
	if (!mapOrphanTransactions.count(hash))
		return;
	const CTransaction& tx = mapOrphanTransactions[hash];
	BOOST_FOREACH(const CTxIn& txin, tx.vin)
	{
		mapOrphanTransactionsByPrev[txin.prevout.hash].erase(hash);
		if (mapOrphanTransactionsByPrev[txin.prevout.hash].empty())
			mapOrphanTransactionsByPrev.erase(txin.prevout.hash);
	}
	mapOrphanTransactions.erase(hash);
		}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
	unsigned int nEvicted = 0;
	while (mapOrphanTransactions.size() > nMaxOrphans)
	{
		// Evict a random orphan:
		uint256 randomhash = GetRandHash();
		map<uint256, CTransaction>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
		if (it == mapOrphanTransactions.end())
			it = mapOrphanTransactions.begin();
		EraseOrphanTx(it->first);
		++nEvicted;
	}
	return nEvicted;
}

bool IsStandardTx(const CTransaction& tx, string& reason)
{
	AssertLockHeld(cs_main);
	if (tx.nVersion > CTransaction::CURRENT_VERSION || tx.nVersion < 1) {
		reason = "version";
		return false;
	}

	// Treat non-final transactions as non-standard to prevent a specific type
	// of double-spend attack, as well as DoS attacks.
	if (!IsFinalTx(tx, chainActive.Height() + 1)) {
		reason = "non-final";
		return false;
	}

	// Limiting transactions to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
	unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
	if (sz >= MAX_STANDARD_TX_SIZE) {
		reason = "tx-size";
		return false;
	}

	BOOST_FOREACH(const CTxIn& txin, tx.vin)
	{
		// Biggest 'standard' txin is a 3-signature 3-of-3 CHECKMULTISIG
		// pay-to-script-hash, which is 3 ~80-byte signatures, 3
		// ~65-byte public keys, plus a few script ops.
		if (txin.scriptSig.size() > 500) {
			reason = "scriptsig-size";
			return false;
		}
		if (!txin.scriptSig.IsPushOnly()) {
			reason = "scriptsig-not-pushonly";
			return false;
		}
		if (!txin.scriptSig.HasCanonicalPushes()) {
			reason = "scriptsig-non-canonical-push";
			return false;
		}
	}

	unsigned int nDataOut = 0;
	txnouttype whichType;
	BOOST_FOREACH(const CTxOut& txout, tx.vout) {
		if (!::IsStandard(txout.scriptPubKey, whichType)) {
			reason = "scriptpubkey";
			return false;
		}
		if (whichType == TX_NULL_DATA)
			nDataOut++;
		else if (txout.IsDust(CTransaction::nMinRelayTxFee)) {
			reason = "dust";
			return false;
		}
	}

	// only one OP_RETURN txout is permitted
	if (nDataOut > 1) {
		reason = "multi-op-return";
		return false;
	}

	return true;
}

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
	AssertLockHeld(cs_main);
	// Time based nLockTime implemented in 0.1.6
	if (tx.nLockTime == 0)
		return true;
	if (nBlockHeight == 0)
		nBlockHeight = chainActive.Height();
	if (nBlockTime == 0)
		nBlockTime = GetAdjustedTime();
	if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
		return true;
	BOOST_FOREACH(const CTxIn& txin, tx.vin)
	if (!txin.IsFinal())
		return false;
	return true;
}

//
// Check transaction inputs to mitigate two
// potential denial-of-service attacks:
//
// 1. scriptSigs with extra data stuffed into them,
//    not consumed by scriptPubKey (or P2SH script)
// 2. P2SH scripts with a crazy number of expensive
//    CHECKSIG/CHECKMULTISIG operations
//
bool AreInputsStandard(const CTransaction& tx, CCoinsViewCache& mapInputs)
{
	if (tx.IsCoinBase())
		return true; // Coinbases don't use vin normally

	for (unsigned int i = 0; i < tx.vin.size(); i++)
	{
		const CTxOut& prev = mapInputs.GetOutputFor(tx.vin[i]);

		vector<vector<unsigned char> > vSolutions;
		txnouttype whichType;
		// get the scriptPubKey corresponding to this input:
		const CScript& prevScript = prev.scriptPubKey;
		if (!Solver(prevScript, whichType, vSolutions))
			return false;
		int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
		if (nArgsExpected < 0)
			return false;

		// Transactions with extra stuff in their scriptSigs are
		// non-standard. Note that this EvalScript() call will
		// be quick, because if there are any operations
		// beside "push data" in the scriptSig
		// IsStandard() will have already returned false
    // and this method isn't called.
		vector<vector<unsigned char> > stack;
		if (!EvalScript(stack, tx.vin[i].scriptSig, tx, i, false, 0))
			return false;

		if (whichType == TX_SCRIPTHASH)
		{
			if (stack.empty())
				return false;
			CScript subscript(stack.back().begin(), stack.back().end());
			vector<vector<unsigned char> > vSolutions2;
			txnouttype whichType2;
      if (Solver(subscript, whichType2, vSolutions2))
      {
          int tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
          if (tmpExpected < 0)
              return false;
          nArgsExpected += tmpExpected;
      }
      else
      {
          // Any other Script with less than 15 sigops OK:
          unsigned int sigops = subscript.GetSigOpCount(true);
          // ... extra data left on the stack after execution is OK, too:
          return (sigops <= MAX_P2SH_SIGOPS);
      }
		}

		if (stack.size() != (unsigned int)nArgsExpected)
			return false;
	}

	return true;
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
	unsigned int nSigOps = 0;
	BOOST_FOREACH(const CTxIn& txin, tx.vin)
	{
		nSigOps += txin.scriptSig.GetSigOpCount(false);
	}
	BOOST_FOREACH(const CTxOut& txout, tx.vout)
	{
		nSigOps += txout.scriptPubKey.GetSigOpCount(false);
	}
	return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, CCoinsViewCache& inputs)
{
	if (tx.IsCoinBase())
		return 0;

	unsigned int nSigOps = 0;
	for (unsigned int i = 0; i < tx.vin.size(); i++)
	{
		const CTxOut &prevout = inputs.GetOutputFor(tx.vin[i]);
		if (prevout.scriptPubKey.IsPayToScriptHash())
			nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
	}
	return nSigOps;
}

bool CheckTransaction(const CTransaction& tx, CValidationState &state)
{
	// Basic checks that don't depend on any context
	if (tx.vin.empty())
		return state.DoS(10, error("CheckTransaction() : vin empty"),
				REJECT_INVALID, "bad-txns-vin-empty");
	if (tx.vout.empty())
		return state.DoS(10, error("CheckTransaction() : vout empty"),
				REJECT_INVALID, "bad-txns-vout-empty");

	unsigned int maxBlockSize;

	GetMaxBlockSizeByTx(tx.GetHash(),maxBlockSize);

	// Size limits
	if (::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION) > maxBlockSize)
		return state.DoS(100, error("CheckTransaction() : size limits failed"),REJECT_INVALID, "bad-txns-oversize");

	// Check for negative or overflow output values
	int64_t nValueOut = 0;
	BOOST_FOREACH(const CTxOut& txout, tx.vout)
	{
		if (txout.nValue < 0)
			return state.DoS(100, error("CheckTransaction() : txout.nValue negative"),
					REJECT_INVALID, "bad-txns-vout-negative");
		if (txout.nValue > MAX_MONEY)
			return state.DoS(100, error("CheckTransaction() : txout.nValue too high"),
					REJECT_INVALID, "bad-txns-vout-toolarge");
		nValueOut += txout.nValue;
		if (!MoneyRange(nValueOut))
			return state.DoS(100, error("CheckTransaction() : txout total out of range"),
					REJECT_INVALID, "bad-txns-txouttotal-toolarge");
	}

	// Check for duplicate inputs
	set<COutPoint> vInOutPoints;
	BOOST_FOREACH(const CTxIn& txin, tx.vin)
	{
		if (vInOutPoints.count(txin.prevout))
			return state.DoS(100, error("CheckTransaction() : duplicate inputs"),
					REJECT_INVALID, "bad-txns-inputs-duplicate");
		vInOutPoints.insert(txin.prevout);
	}

	if (tx.IsCoinBase())
	{
		if (tx.vin[0].scriptSig.size() < 2 || tx.vin[0].scriptSig.size() > 100)
			return state.DoS(100, error("CheckTransaction() : coinbase script size"),
					REJECT_INVALID, "bad-cb-length");
	}
	else
	{
		BOOST_FOREACH(const CTxIn& txin, tx.vin)
    						if (txin.prevout.IsNull())
    							return state.DoS(10, error("CheckTransaction() : prevout is null"),
    									REJECT_INVALID, "bad-txns-prevout-null");
	}

	return true;
}

int64_t GetMinFee(const CTransaction& tx, unsigned int nBytes, bool fAllowFree, enum GetMinFee_mode mode)
{
	// Base fee is either nMinTxFee or nMinRelayTxFee
	int64_t nBaseFee = (mode == GMF_RELAY) ? tx.nMinRelayTxFee : tx.nMinTxFee;

	int64_t nMinFee = (1 + (int64_t)nBytes / 1000) * nBaseFee;

	if (fAllowFree)
	{
		// There is a free transaction area in blocks created by most miners,
		// * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
		//   to be considered to fall into this category. We don't want to encourage sending
		//   multiple transactions instead of one big transaction to avoid fees.
		// * If we are creating a transaction we allow transactions up to 1,000 bytes
		//   to be considered safe and assume they can likely make it into this section.
		if (nBytes < (mode == GMF_SEND ? 1000 : (DEFAULT_BLOCK_PRIORITY_SIZE - 1000)))
			nMinFee = 0;
	}

	// This code can be removed after enough miners have upgraded to version 0.9.
	// Until then, be safe when sending and require a fee if any output
	// is less than CENT:
	if (nMinFee < nBaseFee && mode == GMF_SEND)
	{
		BOOST_FOREACH(const CTxOut& txout, tx.vout)
    						if (txout.nValue < CENT)
    							nMinFee = nBaseFee;
	}

	if (!MoneyRange(nMinFee))
		nMinFee = MAX_MONEY;
	return nMinFee;
}


bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState &state, const CTransaction &tx, bool fLimitFree,
		bool* pfMissingInputs, bool fRejectInsaneFee)
{
	AssertLockHeld(cs_main);
	if (pfMissingInputs)
		*pfMissingInputs = false;

	if (!CheckTransaction(tx, state))
		return error("AcceptToMemoryPool: : CheckTransaction failed");

	// Coinbase is only valid in a block, not as a loose transaction
	if (tx.IsCoinBase())
		return state.DoS(100, error("AcceptToMemoryPool: : coinbase as individual tx"),
				REJECT_INVALID, "coinbase");

	// Rather not work on nonstandard transactions (unless -testnet/-regtest)
	string reason;
	if (Params().NetworkID() == CChainParams::MAIN && !IsStandardTx(tx, reason))
		return state.DoS(0, error("AcceptToMemoryPool : nonstandard transaction: %s", reason), REJECT_NONSTANDARD, reason);

	// is it already in the memory pool?
	uint256 hash = tx.GetHash();
	if (pool.exists(hash))
		return false;

	// Check for conflicts with in-memory transactions
	{
		LOCK(pool.cs); // protect pool.mapNextTx
		for (unsigned int i = 0; i < tx.vin.size(); i++)
		{
			COutPoint outpoint = tx.vin[i].prevout;
			if (pool.mapNextTx.count(outpoint))
			{
				// Disable replacement feature for now
				return false;
			}
		}
	}

	{
		CCoinsView dummy;
		CCoinsViewCache view(dummy);

		{
			LOCK(pool.cs);
			CCoinsViewMemPool viewMemPool(*pcoinsTip, pool);
			view.SetBackend(viewMemPool);

			// do we already have it?
			if (view.HaveCoins(hash))
				return false;

			// Check for all inputs.
			BOOST_FOREACH(const CTxIn txin, tx.vin) {
				if (!view.HaveCoins(txin.prevout.hash)) {
					if (pfMissingInputs)
						*pfMissingInputs = true;
					return false;
				}
			}

			// are the actual inputs available?
			if (!view.HaveInputs(tx))
				return state.Invalid(error("AcceptToMemoryPool : inputs already spent"),REJECT_DUPLICATE, "bad-txns-inputs-spent");

			// Bring the best block into scope
			view.GetBestBlock();

			// we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
			view.SetBackend(dummy);
		}

		// Check for non-standard pay-to-script-hash in inputs
		if (Params().NetworkID() == CChainParams::MAIN && !AreInputsStandard(tx, view))
			return error("AcceptToMemoryPool: : nonstandard transaction input");

		int64_t nValueIn = view.GetValueIn(tx);
		int64_t nValueOut = tx.GetValueOut();
		int64_t nFees = nValueIn-nValueOut;
		double dPriority = view.GetPriority(tx, chainActive.Height());

		// See if transaction contains OP_RETURN output
		if (ContainsOpReturn(tx)){ //
			// Only allow OP_RETURN transactions that send atleast txMinFee amount
			if (nValueOut < DEFAULT_TRANSACTION_FEE)
				return state.DoS(0, error("AcceptToMemoryPool : not enough value sent for OP_RETURN transaction %s, %d < %d", hash.ToString(), nValueOut, DEFAULT_TRANSACTION_FEE), REJECT_INSUFFICIENTFEE, "insufficient output value");
		}
		

		CTxMemPoolEntry entry(tx, nFees, GetTime(), dPriority, chainActive.Height());
		unsigned int nSize = entry.GetTxSize();

		// Don't accept it if it can't get into a block
		int64_t txMinFee = GetMinFee(tx, nSize, true, GMF_RELAY);
		if (fLimitFree && nFees < txMinFee)
			return state.DoS(0, error("AcceptToMemoryPool : not enough fees %s, %d < %d", hash.ToString(), nFees, txMinFee), REJECT_INSUFFICIENTFEE, "insufficient fee");

		// Continuously rate-limit free transactions to mitigate micro-transaction flooding.
		if (fLimitFree && nFees < CTransaction::nMinRelayTxFee)
		{
			static CCriticalSection csFreeLimiter;
			static double dFreeCount;
			static int64_t nLastTime;
			int64_t nNow = GetTime();

			LOCK(csFreeLimiter);

			// Use an exponentially decaying ~10-minute window:
			dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
			nLastTime = nNow;
			// -limitfreerelay unit is thousand-bytes-per-minute
			// At default rate it would take over a month to fill 1GB
			if (dFreeCount >= GetArg("-limitfreerelay", 15)*10*1000)
				return state.DoS(0, error("AcceptToMemoryPool : free transaction rejected by rate limiter"), REJECT_INSUFFICIENTFEE, "insufficient priority");
			LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
			dFreeCount += nSize;
		}


		if (fRejectInsaneFee && nFees > CTransaction::nMinRelayTxFee * 10000)
			return error("AcceptToMemoryPool: : insane fees %s, %d > %d", hash.ToString(), nFees, CTransaction::nMinRelayTxFee * 10000);

		// Check against previous transactions
		// This is done last to help prevent CPU exhaustion denial-of-service attacks.
		if (!CheckInputs(tx, state, view, true, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC))
		{
			return error("AcceptToMemoryPool: : ConnectInputs failed %s", hash.ToString());
		}
		// Store transaction in memory
		pool.addUnchecked(hash, entry);
	}

	g_signals.SyncTransaction(hash, tx, NULL);

	return true;
}

static unsigned int GetMaxBlockSize(unsigned int height)
{
	return MAX_BLOCK_SIZE;
}

//hash ... transaction hash
void GetMaxBlockSizeByTx(const uint256 &hash, unsigned int &maxBlockSize)
{
	unsigned int height;

	if(GetBlockHeightByTx(hash,height))
	{
		maxBlockSize=GetMaxBlockSize(height);
	}
	else
	{
		maxBlockSize=GetMaxBlockSize(chainActive.Height());
	}
}

//hash ... transaction hash
bool GetBlockHeightByTx(const uint256 &hash, unsigned int &height)
{
	height = -1;

	LOCK(cs_main);
	{
		CTransaction tx;
		if (mempool.lookup(hash, tx))
		{
			return false;
		}
	}

	if(fTxIndex)
	{
		CDiskTxPos postx;
		if (pblocktree->ReadTxIndex(hash, postx)) {
			CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
			CBlockHeader header;
			try {
				file >> header;
				//fseek(file, postx.nTxOffset, SEEK_CUR);
				//file >> txOut;
			} catch (std::exception &e) {
				return error("%s : Deserialize or I/O error - %s", __func__, e.what());
			}
			//if (txOut.GetHash() != hash)
			//return error("%s : txid mismatch", __func__);
			height=mapBlockIndex[header.GetHash()]->nHeight;
			return true;
		}
	}

	// use coin database to locate block that contains transaction, and scan it
	CCoinsViewCache &view = *pcoinsTip;
	CCoins coins;
	if (view.GetCoins(hash, coins))
	{
		height = coins.nHeight;
		return true;
	}

	return false;
}


// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &txOut, uint256 &hashBlock, bool fAllowSlow)
{
	CBlockIndex *pindexSlow = NULL;
	{
		LOCK(cs_main);
		{
			if (mempool.lookup(hash, txOut))
			{
				return true;
			}
		}

		if (fTxIndex) {
			CDiskTxPos postx;
			if (pblocktree->ReadTxIndex(hash, postx)) {
				CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
				CBlockHeader header;
				try {
					file >> header;
					fseek(file, postx.nTxOffset, SEEK_CUR);
					file >> txOut;
				} catch (std::exception &e) {
					return error("%s : Deserialize or I/O error - %s", __func__, e.what());
				}
				hashBlock = header.GetHash();
				if (txOut.GetHash() != hash)
					return error("%s : txid mismatch", __func__);
				return true;
			}
		}

		if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
			int nHeight = -1;
			{
				CCoinsViewCache &view = *pcoinsTip;
				CCoins coins;
				if (view.GetCoins(hash, coins))
					nHeight = coins.nHeight;
			}
			if (nHeight > 0)
				pindexSlow = chainActive[nHeight];
		}
	}

	if (pindexSlow) {
		CBlock block;
		if (ReadBlockFromDisk(block, pindexSlow)) {
			BOOST_FOREACH(const CTransaction &tx, block.vtx) {
				if (tx.GetHash() == hash) {
					txOut = tx;
					hashBlock = pindexSlow->GetBlockHash();
					return true;
				}
			}
		}
	}
	return false;
}

//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

bool WriteBlockToDisk(CBlock& block, CDiskBlockPos& pos)
{
	// Open history file to append
	CAutoFile fileout = CAutoFile(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
	if (!fileout)
		return error("WriteBlockToDisk : OpenBlockFile failed");

	// Write index header
	unsigned int nSize = fileout.GetSerializeSize(block);
	fileout << FLATDATA(Params().MessageStart()) << nSize;

	// Write block
	long fileOutPos = ftell(fileout);
	if (fileOutPos < 0)
		return error("WriteBlockToDisk : ftell failed");
	pos.nPos = (unsigned int)fileOutPos;
	fileout << block;

	// Flush stdio buffers and commit to disk before returning
	fflush(fileout);
	if (!IsInitialBlockDownload())
		FileCommit(fileout);

	return true;
}

bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos)
{
	block.SetNull();

	// Open history file to read
	CAutoFile filein = CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
	if (!filein)
		return error("ReadBlockFromDisk : OpenBlockFile failed");

	// Read block
	try {
		filein >> block;
	}
	catch (std::exception &e) {
		return error("%s : Deserialize or I/O error - %s", __func__, e.what());
	}

	// Check the header
	if (!CheckProofOfWork(block.GetPoWHash(block.GetAlgo()), block.nBits, block.GetAlgo()))
		return error("ReadBlockFromDisk : Errors in block header");

	return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex)
{
	if (!ReadBlockFromDisk(block, pindex->GetBlockPos()))
		return false;
	if (block.GetHash() != pindex->GetBlockHash())
		return error("ReadBlockFromDisk(CBlock&, CBlockIndex*) : GetHash() doesn't match index");
	return true;
}

uint256 static GetOrphanRoot(const uint256& hash)
						  {
	map<uint256, COrphanBlock*>::iterator it = mapOrphanBlocks.find(hash);
	if (it == mapOrphanBlocks.end())
		return hash;

	// Work back to the first block in the orphan chain
	do {
		map<uint256, COrphanBlock*>::iterator it2 = mapOrphanBlocks.find(it->second->hashPrev);
		if (it2 == mapOrphanBlocks.end())
			return it->first;
		it = it2;
	} while(true);
						  }

// Remove a random orphan block (which does not have any dependent orphans).
void static PruneOrphanBlocks()
						  {
	if (mapOrphanBlocksByPrev.size() <= MAX_ORPHAN_BLOCKS)
		return;

	// Pick a random orphan block.
	int pos = insecure_rand() % mapOrphanBlocksByPrev.size();
	std::multimap<uint256, COrphanBlock*>::iterator it = mapOrphanBlocksByPrev.begin();
	while (pos--) it++;

	// As long as this block has other orphans depending on it, move to one of those successors.
	do {
		std::multimap<uint256, COrphanBlock*>::iterator it2 = mapOrphanBlocksByPrev.find(it->second->hashBlock);
		if (it2 == mapOrphanBlocksByPrev.end())
			break;
		it = it2;
	} while(1);

	uint256 hash = it->second->hashBlock;
	delete it->second;
	mapOrphanBlocksByPrev.erase(it);
	mapOrphanBlocks.erase(hash);
						  }

const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, int algo)
{
	while (pindex && pindex->pprev && (pindex->GetAlgo() != algo))
		pindex = pindex->pprev;
	return pindex;
}

const CBlockIndex* GetLastBlockIndexForAlgo(const CBlockIndex* pindex, int algo)
{
	for (;;)
	{
		if (!pindex)
			return NULL;
		if (pindex->GetAlgo() == algo)
			return pindex;
		pindex = pindex->pprev;
	}
	return NULL;
}

// The coinbase was split at nRichForkHeight
// 10% goes to the miner while 45% goes to EIAS donation addresses and 45% to dividends

int64_t GetBlockValue(int nHeight, int64_t nFees)
{
	int64_t nSubsidy = 1000 * COIN;

	if(nHeight < nRichForkHeight) // fork height was less than 1226400
		nSubsidy = ((nHeight <= 1000) ? 24000000 : 10000) * COIN; // Premine: First 1K blocks@24M SMLY will give 24 billion SMLY
	else 
		nSubsidy >>= (nHeight / 1226400); // Subsidy is cut in half every 1226400 blocks, which will occur approximately every 7 years

    return nSubsidy + nFees;
}

int64_t GetBlockValueDividends(int nHeight)
{
    int64_t nDividends = 0;  // dividends were not payed before the fork
    if(nHeight >= nRichForkHeight)
        nDividends = 4500 * COIN;
    
    // Subsidy is cut in half every 1226400 blocks, which will occur approximately every 7 years
    nDividends >>= (nHeight / 1226400);
    
    return nDividends;
}

unsigned int static GetNextWorkRequired_Original(const CBlockIndex* pindexLast, const CBlockHeader *pblock, int algo)
{
    unsigned int nProofOfWorkLimit = Params().ProofOfWorkLimit(algo).GetCompact();

    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    if (pindexLast->nHeight+1 < 135)
        return nProofOfWorkLimit;

    nTargetTimespan =  3 * 60 * 60; // 3 hours

    if (pindexLast->nHeight < 97050) {
    	nTargetTimespan =  5 * 24 * 60 * 60; // 5 days before block 97050
    }
    nInterval = nTargetTimespan / nTargetSpacing; // 8

    // Only change once per interval
    if ((pindexLast->nHeight+1) % nInterval != 0)
    {
        // Special difficulty rule for testnet:
        if (TestNet())
        {
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->nTime > pindexLast->nTime + nTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % nInterval != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }
        return pindexLast->nBits;
    }

    // 51% mitigation, courtesy of Art Forz
    int blockstogoback = nInterval-1;
    if ((pindexLast->nHeight+1) != nInterval)
        blockstogoback = nInterval;

    // Go back by what we want to be 14 days worth of blocks
    const CBlockIndex* pindexFirst = pindexLast;
    for (int i = 0; pindexFirst && i < blockstogoback; i++)
        pindexFirst = pindexFirst->pprev;
    assert(pindexFirst);

    // Limit adjustment step
    int64_t nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
    LogPrintf("  nActualTimespan = %d  before bounds\n", nActualTimespan);

    int64_t	nActualTimespanMax = nTargetTimespan*4;
    int64_t	nActualTimespanMin = nTargetTimespan/4;
                  
    if (nActualTimespan < nActualTimespanMin)
        nActualTimespan = nActualTimespanMin;
    if (nActualTimespan > nActualTimespanMax)
        nActualTimespan = nActualTimespanMax;

    // Retarget
    CBigNum bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= nActualTimespan;
    bnNew /= nTargetTimespan;

    if (bnNew > Params().ProofOfWorkLimit(algo))
          bnNew = Params().ProofOfWorkLimit(algo);

      /// debug print
    LogPrintf("GetNextWorkRequired RETARGET\n");
    LogPrintf("nTargetTimespan = %d    nActualTimespan = %d \n", nTargetTimespan, nActualTimespan);
    LogPrintf("Before: %08x  %s\n", pindexLast->nBits, CBigNum().SetCompact(pindexLast->nBits).getuint256().ToString().c_str());
    LogPrintf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.getuint256().ToString().c_str());

    LogPrintf("RETARGET %d  %d  %08x  %08x  %s\n", nTargetTimespan, nActualTimespan, pindexLast->nBits, bnNew.GetCompact(), bnNew.getuint256().ToString().c_str());

    return bnNew.GetCompact();
 }


static unsigned int GetNextWorkRequiredMULTI(const CBlockIndex* pindexLast, const CBlockHeader *pblock, int algo)
{
	unsigned int nProofOfWorkLimit = Params().ProofOfWorkLimit(algo).GetCompact();

	//if (TestNet())
	//{
	//	// Testnet minimum difficulty block if it goes over normal block time.
	//	if (pblock->nTime > pindexLast->nTime + nTargetSpacing*2)
	//		return nProofOfWorkLimit;
	//	else
	//	{
	//		// Return the last non-special-min-difficulty-rules-block
	//		const CBlockIndex* pindex = pindexLast;
	//		while (pindex->pprev && pindex->nHeight % nInterval != 0 && pindex->nBits == nProofOfWorkLimit)
	//			pindex = pindex->pprev;
	//		return pindex->nBits;
	//	}
	//}

	//LogPrintf("GetNextWorkRequired RETARGET\n");
	//LogPrintf("Algo: %s\n", GetAlgoName(algo));
	//LogPrintf("Height (Before): %s\n", pindexLast->nHeight);

	// find first block in averaging interval
	// Go back by what we want to be nAveragingInterval blocks per algo
	const CBlockIndex* pindexFirst = pindexLast;
	const bool fDiffChange = pindexLast->nHeight >= nRichForkV2Height;
	for (int i = 0; pindexFirst && i < NUM_ALGOS*(fDiffChange ? nAveragingIntervalV2 : nAveragingInterval); i++)
	{
		pindexFirst = pindexFirst->pprev;
	}

	const CBlockIndex* pindexPrevAlgo = GetLastBlockIndexForAlgo(pindexLast, algo);
	if (pindexPrevAlgo == NULL || pindexFirst == NULL)
	{
		//LogPrintf("Use default POW Limit\n");
		return nProofOfWorkLimit;
	}

	multiAlgoTimespan = (pindexLast->nHeight >= 222000) ? 180 : 36;    
    multiAlgoTargetSpacing = multiAlgoNum * multiAlgoTimespan; // 5 * 180(36) seconds = 900 seconds

	// Limit adjustment step
	// Use medians to prevent time-warp attacks
	int64_t nActualTimespan = pindexLast-> GetMedianTimePast() - pindexFirst->GetMedianTimePast();
	// nAveragingTargetTimespan was initially not initialized correctly (heh)
	if(pindexLast->nHeight < nRichForkV2Height) {
		nActualTimespan = nAveragingTargetTimespan + (nActualTimespan - nAveragingTargetTimespan)/4;
		nAveragingTargetTimespan = nAveragingInterval * multiAlgoTargetSpacing; // 60* 5 * 180 = 54000 seconds
	} else {
		nAveragingTargetTimespan = nAveragingIntervalV2 * multiAlgoTargetSpacing; // 2* 5 * 180 = 1800 seconds
		nActualTimespan = nAveragingTargetTimespan + (nActualTimespan - nAveragingTargetTimespan)/4;
	}   
 
    nMinActualTimespan = nAveragingTargetTimespan * (100 - (fDiffChange ? nMaxAdjustUpV2 : nMaxAdjustUp)) / 100;
    nMaxActualTimespan = nAveragingTargetTimespan * (100 + (fDiffChange ? nMaxAdjustDownV2 : nMaxAdjustDown)) / 100;
    
    if (nActualTimespan < nMinActualTimespan)
		nActualTimespan = nMinActualTimespan;
	if (nActualTimespan > nMaxActualTimespan)
		nActualTimespan = nMaxActualTimespan;

	//Global retarget
	CBigNum bnNew;
	bnNew.SetCompact(pindexPrevAlgo->nBits);

	bnNew *= nActualTimespan;
	bnNew /= nAveragingTargetTimespan;

	//Per-algo retarget
	int nAdjustments = pindexPrevAlgo->nHeight + NUM_ALGOS - 1 - pindexLast->nHeight;
	if (nAdjustments > 0)
	{
		for (int i = 0; i < nAdjustments; i++)
		{
			bnNew *= 100;
			bnNew /= (100 + (fDiffChange ? nLocalTargetAdjustmentV2 : nLocalTargetAdjustment));
		}
	}
	else if (nAdjustments < 0)//make it easier
	{
		for (int i = 0; i < -nAdjustments; i++)
		{
			bnNew *= (100 + (fDiffChange ? nLocalTargetAdjustmentV2 : nLocalTargetAdjustment));
			bnNew /= 100;
		}
	}

	if(bnNew > Params().ProofOfWorkLimit(algo))
	{
		if(!IsInitialBlockDownload())
			LogPrintf("New nBits below minimum work: Use default POW Limit\n");
		return nProofOfWorkLimit;
	}

  //LogPrintf("MULTI %d  %d  %08x  %08x  %s\n", multiAlgoTimespan, nActualTimespan, pindexLast->nBits, bnNew.GetCompact(), bnNew.getuint256().ToString().c_str());

	return bnNew.GetCompact();
}


unsigned int GetNextWorkRequired(const CBlockIndex * pindexLast, const CBlockHeader * pblock, int algo)
{
    if (TestNet()) {
        if (pindexLast->nHeight+1 < 50) {
            return GetNextWorkRequired_Original(pindexLast, pblock, algo);
        }
        else {
            return GetNextWorkRequiredMULTI(pindexLast, pblock, algo);
        }
    }
    else {
        if (pindexLast->nHeight+1 < nRichForkHeight) {
            return GetNextWorkRequired_Original(pindexLast, pblock, algo);
        }
        else {
            return GetNextWorkRequiredMULTI(pindexLast, pblock, algo);
        }
    }
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits, int algo)
{
	CBigNum bnTarget;
	bnTarget.SetCompact(nBits);

	// Check range
	if (bnTarget <= 0 || bnTarget > Params().ProofOfWorkLimit(algo))
		return error("CheckProofOfWork(algo=%d) : nBits (%08x) below minimum work (%08x)", algo, bnTarget.GetCompact(), Params().ProofOfWorkLimit(algo).GetCompact());

	// Check proof of work matches claimed amount
	if (hash > bnTarget.getuint256())
		return error("CheckProofOfWork(algo=%d) : hash doesn't match nBits", algo);

	return true;
}

// Return maximum amount of blocks that other nodes claim to have
int GetNumBlocksOfPeers()
{
	return std::max(cPeerBlockCounts.median(), Checkpoints::GetTotalBlocksEstimate());
}

bool IsInitialBlockDownload()
{
	LOCK(cs_main);
	if (fImporting || fReindex || chainActive.Height() < Checkpoints::GetTotalBlocksEstimate())
		return true;
	static int64_t nLastUpdate;
	static CBlockIndex* pindexLastBest;
	if (chainActive.Tip() != pindexLastBest)
	{
		pindexLastBest = chainActive.Tip();
		nLastUpdate = GetTime();
	}
	return (GetTime() - nLastUpdate < 10 &&
			chainActive.Tip()->GetBlockTime() < GetTime() - 24 * 60 * 60);
}

bool fLargeWorkForkFound = false;
bool fLargeWorkInvalidChainFound = false;
CBlockIndex *pindexBestForkTip = NULL, *pindexBestForkBase = NULL;

void CheckForkWarningConditions()
{
	AssertLockHeld(cs_main);
	// Before we get past initial download, we cannot reliably alert about forks
	// (we assume we don't get stuck on a fork before the last checkpoint)
	if (IsInitialBlockDownload())
		return;

	// Drop the best fork if it is no longer within 72 blocks of our head
	if (pindexBestForkTip && chainActive.Height() - pindexBestForkTip->nHeight >= 72)
		pindexBestForkTip = NULL;

	if (pindexBestForkTip
			|| (pindexBestInvalid
					&& pindexBestInvalid->nChainWork > chainActive.Tip()->nChainWork + (chainActive.Tip()->GetBlockWorkAdjusted() * 6).getuint256()
					&& (chainActive.Height() > pindexBestInvalid->nHeight + 3 || pindexBestInvalid->nHeight > chainActive.Height() + 3)
			)
	)
	{
		if (!fLargeWorkForkFound)
		{
			std::string strCmd = GetArg("-alertnotify", "");
			if (!strCmd.empty())
			{
				std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") + pindexBestForkBase->phashBlock->ToString() + std::string("'");
				boost::replace_all(strCmd, "%s", warning);
				boost::thread t(runCommand, strCmd); // thread runs free
			}
		}
		if (pindexBestForkTip)
		{
			LogPrintf("CheckForkWarningConditions: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n",
					pindexBestForkBase->nHeight, pindexBestForkBase->phashBlock->ToString(),
					pindexBestForkTip->nHeight, pindexBestForkTip->phashBlock->ToString());
			fLargeWorkForkFound = true;
		}
		else
		{
			LogPrintf("CheckForkWarningConditions: Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.\n");
			fLargeWorkInvalidChainFound = true;
		}
	}
	else
	{
		fLargeWorkForkFound = false;
		fLargeWorkInvalidChainFound = false;
	}
}

void CheckForkWarningConditionsOnNewFork(CBlockIndex* pindexNewForkTip)
{
	AssertLockHeld(cs_main);
	// If we are on a fork that is sufficiently large, set a warning flag
	CBlockIndex* pfork = pindexNewForkTip;
	CBlockIndex* plonger = chainActive.Tip();
	while (pfork && pfork != plonger)
	{
		while (plonger && plonger->nHeight > pfork->nHeight)
			plonger = plonger->pprev;
		if (pfork == plonger)
			break;
		pfork = pfork->pprev;
	}

	// We define a condition which we should warn the user about as a fork of at least 20 blocks
	// who's tip is within 72 blocks (+/- 12 hours if no one mines it) of ours
	if (pfork && (!pindexBestForkTip || (pindexBestForkTip && pindexNewForkTip->nHeight > pindexBestForkTip->nHeight)) &&
			pindexNewForkTip->nChainWork - pfork->nChainWork > (pfork->GetBlockWorkAdjusted() * 20).getuint256() &&
			chainActive.Height() - pindexNewForkTip->nHeight < 72)
	{
		pindexBestForkTip = pindexNewForkTip;
		pindexBestForkBase = pfork;
	}

	CheckForkWarningConditions();
}

// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch)
{
	if (howmuch == 0)
		return;

	CNodeState *state = State(pnode);
	if (state == NULL)
		return;

	state->nMisbehavior += howmuch;
	if (state->nMisbehavior >= GetArg("-banscore", 100))
	{
		LogPrintf("Misbehaving: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", state->name, state->nMisbehavior-howmuch, state->nMisbehavior);
		state->fShouldBan = true;
	} else
		LogPrintf("Misbehaving: %s (%d -> %d)\n", state->name, state->nMisbehavior-howmuch, state->nMisbehavior);
}

void static InvalidChainFound(CBlockIndex* pindexNew)
						  {
	if (!pindexBestInvalid || pindexNew->nChainWork > pindexBestInvalid->nChainWork)
	{
		pindexBestInvalid = pindexNew;
		// BestInvalidWork for backward compatibility.
		pblocktree->WriteBestInvalidWork(CBigNum(pindexBestInvalid->nChainWork));
		uiInterface.NotifyBlocksChanged();
	}
	LogPrintf("InvalidChainFound: invalid block=%s  height=%d  log2_work=%.8g  date=%s\n",
			pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
			log(pindexNew->nChainWork.getdouble())/log(2.0), DateTimeStrFormat("%Y-%m-%d %H:%M:%S",
					pindexNew->GetBlockTime()));
	LogPrintf("InvalidChainFound:  current best=%s  height=%d  log2_work=%.8g  date=%s\n",
			chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(), log(chainActive.Tip()->nChainWork.getdouble())/log(2.0),
			DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()));
	CheckForkWarningConditions();
						  }

void static InvalidBlockFound(CBlockIndex *pindex, const CValidationState &state) {
	int nDoS = 0;
	if (state.IsInvalid(nDoS)) {
		std::map<uint256, NodeId>::iterator it = mapBlockSource.find(pindex->GetBlockHash());
		if (it != mapBlockSource.end() && State(it->second)) {
			CBlockReject reject = {state.GetRejectCode(), state.GetRejectReason(), pindex->GetBlockHash()};
			State(it->second)->rejects.push_back(reject);
			if (nDoS > 0)
				Misbehaving(it->second, nDoS);
		}
	}
	if (!state.CorruptionPossible()) {
		pindex->nStatus |= BLOCK_FAILED_VALID;
		pblocktree->WriteBlockIndex(CDiskBlockIndex(pindex));
		setBlockIndexValid.erase(pindex);
		InvalidChainFound(pindex);
	}
}

void UpdateTime(CBlockHeader& block, const CBlockIndex* pindexPrev)
{
	block.nTime = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

	// Updating time can change work required on testnet:
	//if (TestNet())
	//	block.nBits = GetNextWorkRequired(pindexPrev, &block, block.GetAlgo());
}

void UpdateCoins(const CTransaction& tx, CValidationState &state, CCoinsViewCache &inputs, CTxUndo &txundo, int nHeight, const uint256 &txhash)
{
	bool ret;
	// mark inputs spent
	if (!tx.IsCoinBase()) {
		BOOST_FOREACH(const CTxIn &txin, tx.vin) {
			CCoins &coins = inputs.GetCoins(txin.prevout.hash);
			CTxInUndo undo;
			ret = coins.Spend(txin.prevout, undo);
			assert(ret);
			txundo.vprevout.push_back(undo);
		}
	}

	// add outputs
	ret = inputs.SetCoins(txhash, CCoins(tx, nHeight));
	assert(ret);
}

bool CScriptCheck::operator()() const {
	const CScript &scriptSig = ptxTo->vin[nIn].scriptSig;
	if (!VerifyScript(scriptSig, scriptPubKey, *ptxTo, nIn, nFlags, nHashType))
		return error("CScriptCheck() : %s VerifySignature failed", ptxTo->GetHash().ToString());
	return true;
}

bool VerifySignature(const CCoins& txFrom, const CTransaction& txTo, unsigned int nIn, unsigned int flags, int nHashType)
{
	return CScriptCheck(txFrom, txTo, nIn, flags, nHashType)();
}

bool CheckInputs(const CTransaction& tx, CValidationState &state, CCoinsViewCache &inputs, bool fScriptChecks, unsigned int flags, std::vector<CScriptCheck> *pvChecks)
{
	if (!tx.IsCoinBase())
	{
		if (pvChecks)
			pvChecks->reserve(tx.vin.size());

		// This doesn't trigger the DoS code on purpose; if it did, it would make it easier
		// for an attacker to attempt to split the network.
		if (!inputs.HaveInputs(tx))
			return state.Invalid(error("CheckInputs() : %s inputs unavailable", tx.GetHash().ToString()));

		// While checking, GetBestBlock() refers to the parent block.
		// This is also true for mempool checks.
		CBlockIndex *pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
		int nSpendHeight = pindexPrev->nHeight + 1;
		int64_t nValueIn = 0;
		int64_t nFees = 0;
		for (unsigned int i = 0; i < tx.vin.size(); i++)
		{
			const COutPoint &prevout = tx.vin[i].prevout;
			const CCoins &coins = inputs.GetCoins(prevout.hash);

			// If prev is coinbase, check that it's matured
			if (coins.IsCoinBase()) {
				if (coins.nHeight < nRichForkHeight) { 
					if (nSpendHeight - coins.nHeight < COINBASE_MATURITY)
						return state.Invalid(error("CheckInputs() : tried coinbase at depth %d %d %d %d",
								nSpendHeight - coins.nHeight, nSpendHeight, coins.nHeight, COINBASE_MATURITY));
				}
			}

			// Check for negative or overflow input values
			nValueIn += coins.vout[prevout.n].nValue;
			if (!MoneyRange(coins.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
				return state.DoS(100, error("CheckInputs() : txin values out of range"),REJECT_INVALID, "bad-txns-inputvalues-outofrange");

		}

		if (nValueIn < tx.GetValueOut())
			return state.DoS(100, error("CheckInputs() : %s value in < value out", tx.GetHash().ToString()),
					REJECT_INVALID, "bad-txns-in-belowout");

		// Tally transaction fees
		int64_t nTxFee = nValueIn - tx.GetValueOut();
		if (nTxFee < 0)
			return state.DoS(100, error("CheckInputs() : %s nTxFee < 0", tx.GetHash().ToString()),
					REJECT_INVALID, "bad-txns-fee-negative");
		nFees += nTxFee;
		if (!MoneyRange(nFees))
			return state.DoS(100, error("CheckInputs() : nFees out of range"),
					REJECT_INVALID, "bad-txns-fee-outofrange");

		// Skip ECDSA signature verification when connecting blocks before the last block chain checkpoint.
		if (fScriptChecks) {
			for (unsigned int i = 0; i < tx.vin.size(); i++) {
				const COutPoint &prevout = tx.vin[i].prevout;
				const CCoins &coins = inputs.GetCoins(prevout.hash);

				// Verify signature
				CScriptCheck check(coins, tx, i, flags, 0);
				if (pvChecks) {
					pvChecks->push_back(CScriptCheck());
					check.swap(pvChecks->back());
				} else if (!check()) {
					if (flags & SCRIPT_VERIFY_STRICTENC) {
						// Non-canonical failure check to determine whether to trigger DoS protection.
						CScriptCheck check(coins, tx, i, flags & (~SCRIPT_VERIFY_STRICTENC), 0);
						if (check())
							return state.Invalid(false, REJECT_NONSTANDARD, "non-canonical");
					}
					return state.DoS(100,false, REJECT_NONSTANDARD, "non-canonical");
				}
			}
		}
	}

	return true;
}

bool DisconnectBlock(CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, bool* pfClean)
{
	assert(pindex->GetBlockHash() == view.GetBestBlock());

	if (pfClean)
		*pfClean = false;

    bool fClean = true;

    CBlockUndo blockUndo;
	CDiskBlockPos pos = pindex->GetUndoPos();
	if (pos.IsNull())
		return error("DisconnectBlock() : no undo data available");
	if (!blockUndo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
		return error("DisconnectBlock() : failure reading undo data");

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size())
		return error("DisconnectBlock() : block and undo data inconsistent");

    std::map<CScript, std::pair<int64_t, int> > addressInfo;
    std::map<std::string, std::tuple<std::string, std::string, std::string> > serviceInfo;
	std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> > serviceCouponList;
	std::map<std::string, std::tuple<std::string, std::string> > serviceUbiList;
	std::map<std::string, std::tuple<std::string, std::string, std::string> > serviceDexList;
	std::map<std::string, std::tuple<std::string, std::string, std::string> > serviceBookList;
	std::map<std::string, std::tuple<std::string, std::string, std::string> > serviceNPList;

    // undo transactions in reverse order
	for (int i = block.vtx.size() - 1; i >= 0; i--) {
		const CTransaction &tx = block.vtx[i];
		uint256 hash = tx.GetHash();

		// processing outputs for address index
		for (unsigned int k = 0; k < tx.vout.size(); k++) {
			const CTxOut &out = tx.vout[k];
			const CScript &key = out.scriptPubKey;
			const int64_t &value = out.nValue;

			// check if the output contains op_return, then it might be a service output
			if (ContainsOpReturn(tx)) {
                CTxDestination des;
                ExtractDestination(key, des);
                // for the output to be a service output it has to be sent to B9TRXJzgUJZZ5zPZbywtNfZHeu492WWRxc and have the value 10 SMLY
                if (CBitcoinAddress(des).ToString() == "B9TRXJzgUJZZ5zPZbywtNfZHeu492WWRxc" && value == 10*COIN) {
                    for (unsigned int l = 0; l < tx.vout.size(); l++) {
                        const CTxOut &outService = tx.vout[l];
                        const CScript &keyService = outService.scriptPubKey;
                        std::string hexString = HexStr(keyService);
                        std::string hexData;
                        std::string newService;
                        std::string serviceName;
                        std::string serviceAddress;
                        std::string serviceType;

						// grab the hex data for the op_return
                        if (hexString.substr(0, 2) == "6a") {
                            hexData = hexString.substr(4, hexString.size());

                            // New service
                            // If op_return starts with "NS"
                            if (hexData.substr(0, 4) == "4e53") {
                                newService = hexData.substr(4, hexString.size());
                                std::vector<std::string> strs = splitString(newService, "20");
								// the string has to have 3 params to be valid as a service and saved into the db
                                if (strs.size() == 3) {
                                    serviceName = strs.at(0);
                                    serviceAddress = strs.at(1);
                                    serviceType = strs.at(2);
                                    std::string asciiAddress = hexToAscii(serviceAddress);
                                    CBitcoinAddress sAddress = CBitcoinAddress(asciiAddress);

                                    if (sAddress.IsValid() && !ServiceList.IsService(asciiAddress) && serviceName.length() <= 50 &&
                                    (hexToAscii(serviceType) == "1" || hexToAscii(serviceType) == "2" ||
                                    hexToAscii(serviceType) == "3"|| hexToAscii(serviceType) == "4"||
                                    hexToAscii(serviceType) == "5"|| hexToAscii(serviceType) == "6" || hexToAscii(serviceType) == "7"
                                    || hexToAscii(serviceType) == "8")) {
                                        std::tuple<std::string, std::string, std::string> value;
                                        if (!view.GetServiceInfo(asciiAddress, value)) {
                                            return state.Abort(_("Failed to read service index"));
                                        } else {
                                            value = std::make_tuple("NS", hexToAscii(serviceName), hexToAscii(serviceType));
                                            assert(view.SetServiceInfo(asciiAddress, value));
                                            serviceInfo[asciiAddress] = value;
                                            // refresh the wallet if added into the db
                                            if (pwalletMain) {
                                                pwalletMain->NotifyServicePageChanged(pwalletMain, hexToAscii(serviceName),
                                                        asciiAddress, hexToAscii(serviceType), CT_NEW);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
					// if it is not sent to B9TRXJzgUJZZ5zPZbywtNfZHeu492WWRxc or doesn't have the value 10 SMLY
                    std::string toAddress = CBitcoinAddress(des).ToString();
                    // if the output is sent to a valid service address then check if it is a ubi, dex, book or npo
                    if (CBitcoinAddress(toAddress).IsValid()) {
                        for(unsigned int l = 0; l < tx.vout.size(); l++) {
                            const CTxOut &outService = tx.vout[l];
                            const CScript &keyService = outService.scriptPubKey;
                            std::string hexString = HexStr(keyService);

                            // check if the scriptPubKey contains op_return(6a)
							if (hexString.substr(0, 2) == "6a") {
								std::string hexData = hexString.substr(4, hexString.size());
                                // New coupon
								// If the string starts with "NT"
								if (hexData.substr(0, 4) == "4e54") {
									std::string newCoupon = hexData.substr(4, hexString.size());
									std::vector<std::string> strs = splitString(newCoupon, "20");
									// the string has to have 5 params to be valid as a service and saved into the db
									if (strs.size() == 5) {
										std::string couponLocation = strs.at(0);
										std::string couponName = strs.at(1);
										std::string couponDateAndTime = strs.at(2);
										std::string couponValue = strs.at(3);
										std::string couponAddress = strs.at(4);
                                        std::string asciiAddress = hexToAscii(couponAddress);
                                        CBitcoinAddress tAddress = CBitcoinAddress(asciiAddress);
                                        // if the coupon address is valid, the name and location are equal or less than 40,
                                        // date and time is on the right format and value is an integer then save it to the db
                                        if (tAddress.IsValid() 
                                            && is_before(hexToAscii(couponDateAndTime)) 
                                            && couponLocation.length() <= 40 
                                            && couponName.length() <= 40 
                                            && ServiceList.IsService(toAddress)) {
                                            std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> value;
											if(!view.GetCouponList(asciiAddress, value)) {
												LogPrintf("Failed to read coupon index");
                                                continue;
                                            } else {
												value = std::make_tuple("NT", toAddress, hexToAscii(couponLocation), hexToAscii(couponName), hexToAscii(couponDateAndTime), hexToAscii(couponValue));
												assert(view.SetCouponList(asciiAddress, value));
												serviceCouponList[asciiAddress]=value;
                                                // refresh the wallet when it is saved into the db
												if (pwalletMain)
                                                {
                                                    pwalletMain->NotifyCouponPageChanged(pwalletMain,
                                                                                         hexToAscii(couponName),
                                                                                         hexToAscii(couponLocation),
                                                                                         hexToAscii(couponDateAndTime),
                                                                                         hexToAscii(couponValue),
                                                                                         asciiAddress,
                                                                                         toAddress,
                                                                                         CT_NEW);
                                                }
											}
										}
									}
								}
								// New UBI
								// If the string starts with "NU"
								else if (hexData.substr(0, 4) == "4e55") {
									std::string newUbi = hexData.substr(4, hexString.size());
									std::vector<std::string> strs = splitString(newUbi, "20");
									// the string has to have 1 params to be valid as a ubi and saved into the db
									if (strs.size() == 1) {
										std::string ubiAddress = strs.at(0);
                                        std::string asciiAddress = hexToAscii(ubiAddress);
                                        CBitcoinAddress uAddress = CBitcoinAddress(asciiAddress);
										if (uAddress.IsValid() && ServiceList.IsService(toAddress)) {
											std::tuple<std::string, std::string> value;
											if(!view.GetUbiList(asciiAddress, value)) {
												LogPrintf("Failed to read ubi index");
                                                continue;
                                            } else {
												value = std::make_tuple("NU", toAddress);
												assert(view.SetUbiList(asciiAddress, value));
												serviceUbiList[asciiAddress]=value;
											}
										}
									}
								}
								// New DEX
								// If the string starts with "ND"
								else if (hexData.substr(0, 4) == "4e44") {
									std::string newDex = hexData.substr(4, hexString.size());
									std::vector<std::string> strs = splitString(newDex, "20");
									// The string has to have 2 params to be valid as a dex and saved into the db
									if (strs.size() == 2) {
										std::string dexAddress = strs.at(0);
										std::string dexDescription = strs.at(1);
                                        std::string asciiAddress = hexToAscii(dexAddress);
                                        CBitcoinAddress dAddress = CBitcoinAddress(asciiAddress);
										if (dAddress.IsValid() && dexDescription.length() <= 60 && ServiceList.IsService(toAddress)) {
											std::tuple<std::string, std::string, std::string> value;
											if(!view.GetDexList(asciiAddress, value)) {
												LogPrintf("Failed to read dex index");
                                                continue;
                                            } else {
												value = std::make_tuple("ND", toAddress, hexToAscii(dexDescription));
												assert(view.SetDexList(asciiAddress, value));
												serviceDexList[asciiAddress]=value;
											}
										}
									}
								}
								// New Np
								// If the string starts with "NN"
								else if (hexData.substr(0, 4) == "4e4e") {
									std::string newDex = hexData.substr(4, hexString.size());
									std::vector<std::string> strs = splitString(newDex, "20");
									// The string has to have 2 params to be valid as a np and saved into the db
									if (strs.size() == 2) {
										std::string npAddress = strs.at(0);
										std::string npName = strs.at(1);
                                        std::string asciiAddress = hexToAscii(npAddress);
                                        CBitcoinAddress nAddress = CBitcoinAddress(asciiAddress);
										if (nAddress.IsValid() && npName.length() <= 60 && ServiceList.IsService(toAddress)) {
											std::tuple<std::string, std::string, std::string> value;
											if(!view.GetNPList(asciiAddress, value)) {
												LogPrintf("Failed to read np index");
                                                continue;
                                            } else {
												value = std::make_tuple("NN", toAddress, hexToAscii(npName));
												serviceNPList[asciiAddress]=value;
											}
										}
									}
								}
								// New book chapter
								// If the string starts with "NB"
								else if (hexData.substr(0, 4) == "4e42") {
									std::string newBookChapter = hexData.substr(4, hexString.size());
                                    std::vector<std::string> strs = splitString(newBookChapter, "20");
                                    // The string has to have 2 params to be valid as a book chapter and saved into the db
                                    if (strs.size() == 2) {
                                        std::string chapterNum = strs.at(0);
                                        std::string chapterAddress = strs.at(1);

                                        std::string asciiAddress = hexToAscii(chapterAddress);
                                        CBitcoinAddress bAddress = CBitcoinAddress(asciiAddress);
                                        if (bAddress.IsValid() && ServiceList.IsService(toAddress)) {
                                            std::tuple<std::string, std::string, std::string> value;
                                            if(!view.GetBookList(asciiAddress, value)) {
												LogPrintf("Failed to read book index");
                                                continue;
                                            } else {
                                                value = std::make_tuple("NB", toAddress, hexToAscii(chapterNum));
                                                assert(view.SetBookList(asciiAddress, value));
                                                serviceBookList[asciiAddress]=value;
                                            }
                                        }
                                    }
								}
                                // Delete service
                                // If op_return begins with "DS"
                                else if (hexData.substr(0, 6) == "445320") {
                                    std::string serviceAddress = hexData.substr(6, hexString.size());
                                    std::string asciiAddress = hexToAscii(serviceAddress);
                                    CBitcoinAddress sAddress = CBitcoinAddress(asciiAddress);
                                    if (sAddress.IsValid() && ServiceList.IsService(asciiAddress) && ServiceList.IsService(toAddress)) {
                                        std::tuple<std::string, std::string, std::string> value;
                                        value = std::make_tuple("DS", "", "");
                                        assert(view.SetServiceInfo(asciiAddress, value));
                                        serviceInfo[asciiAddress] = value;

                                        if (pwalletMain) {
                                            pwalletMain->NotifyServicePageChanged(pwalletMain, "", asciiAddress,
                                                    "", CT_DELETED);
                                        }
                                    }
                                }
                                // delete np, opreturn begins with "DN "
                                else if (hexData.substr(0, 6) == "444e20") {
									std::string newDex = hexData.substr(6, hexString.size());
									std::vector<std::string> strs = splitString(newDex, "20");
									// The string has to have 2 params to be valid as a np and saved into the db
									if (strs.size() == 1) {
										std::string npAddress = strs.at(0);
                                        std::string asciiAddress = hexToAscii(npAddress);
                                        CBitcoinAddress nAddress = CBitcoinAddress(asciiAddress);
										if (nAddress.IsValid() && ServiceList.IsService(toAddress)) {
											std::tuple<std::string, std::string, std::string> value;
											if(!view.GetNPList(asciiAddress, value)) {
												LogPrintf("Failed to read np index");
                                                continue;
                                            } else {
												value = std::make_tuple("DN", asciiAddress, "");
												//assert(view.SetNPList(asciiAddress, value));
												serviceNPList[asciiAddress]=value;

                                                if (pwalletMain) {
                                                    pwalletMain->NotifyServicePageChanged(pwalletMain, "", asciiAddress,
                                                            "", CT_DELETED);
                                                }
											}
										}
									}
                                }
                                // Delete coupon
                                // If op_return begins with "DT"
                                else if (hexData.substr(0, 6) == "445420") {
                                    std::string couponAddress = hexData.substr(6, hexString.size());
                                    std::string asciiAddress = hexToAscii(couponAddress);
                                    CBitcoinAddress tAddress = CBitcoinAddress(asciiAddress);
                                    // If the coupon address is valid then save it to the db
                                    if (tAddress.IsValid() && ServiceItemList.IsCoupon(asciiAddress)) {
                                        std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> value;
                                        value = std::make_tuple("DT", toAddress, "", "", "", "");
                                        assert(view.SetCouponList(asciiAddress, value));
                                        serviceCouponList[asciiAddress]=value;

                                        if (pwalletMain)
                                        {
                                            pwalletMain->NotifyCouponPageChanged(pwalletMain, "", "", "", "",
                                                    asciiAddress,
                                                    toAddress,
                                                    CT_DELETED);
                                        }
                                    }
                                }
                                // delete ubi
                                else if (hexData.substr(0, 6) == "445520") {
									std::string newDex = hexData.substr(6, hexString.size());
									std::vector<std::string> strs = splitString(newDex, "20");
									// The string has to have 1 params to be valid as a np and saved into the db
                                    std::string ubiAddress = strs.at(0);
                                    std::string asciiAddress = hexToAscii(ubiAddress);
                                    CBitcoinAddress uAddress = CBitcoinAddress(asciiAddress);
                                    if (uAddress.IsValid() && ServiceList.IsService(toAddress)) {
                                        std::tuple<std::string, std::string> value;
                                        if (!view.GetUbiList(asciiAddress, value)) {
                                            LogPrintf("Failed to read ubi index");
                                            continue;
                                        } else {
                                            value = std::make_tuple("DU", asciiAddress);
                                            serviceUbiList[asciiAddress] = value;

                                            if (pwalletMain) {
                                                pwalletMain->NotifyServicePageChanged(pwalletMain, "", asciiAddress,
                                                        "", CT_DELETED);
                                            }
                                        }
                                    }
                                }
                                // If op_return begins with "BT" (buy coupon)
                                else if (hexData.substr(0, 4) == "4254") {
                                    std::string buyCoupon = hexData.substr(4, hexString.size());
                                    std::vector<std::string> strs = splitString(buyCoupon, "20");
                                    // The string has to have 5 params to be valid as a service and saved into the db
                                    if (strs.size() == 5) {
                                        std::string couponLocation = strs.at(0);
                                        std::string couponName = strs.at(1);
                                        std::string couponDateAndTime = strs.at(2);
                                        std::string couponValue = strs.at(3);
                                        std::string couponAddress = strs.at(4);

                                        std::string asciiAddress = hexToAscii(couponAddress);
                                        CBitcoinAddress tAddress = CBitcoinAddress(asciiAddress);
                                        // If the coupon address is valid then save it to the db
                                        if (tAddress.IsValid() && ServiceItemList.IsCoupon(asciiAddress)) {
                                            std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> value;
                                            value = std::make_tuple("BT", toAddress, hexToAscii(couponLocation), hexToAscii(couponName), hexToAscii(couponDateAndTime), hexToAscii(couponValue));
                                            assert(view.SetCouponList(asciiAddress, value));
                                            serviceCouponList[asciiAddress]=value;

                                            if (pwalletMain)
                                            {
                                                pwalletMain->NotifyCouponPageChanged(pwalletMain,
                                                        hexToAscii(couponName),
                                                        hexToAscii(couponLocation),
                                                        hexToAscii(couponDateAndTime),
                                                        hexToAscii(couponValue),
                                                        asciiAddress,
                                                        toAddress,
                                                        CT_DELETED);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (key.IsPayToPublicKeyHash() || key.IsPayToScriptHash())
			{
				std::pair<int64_t, int> value;
				if(!view.GetAddressInfo(key,value))
					return state.Abort(_("Failed to read address index"));
				else {
					value = std::make_pair(value.first - out.nValue, pindex->nHeight);
					assert(view.SetAddressInfo(key,value));
					addressInfo[key]=value;
				}
			}
		}

		// Check that all outputs are available and match the outputs in the block itself exactly.
		CCoins outsEmpty;
		CCoins &outs = view.HaveCoins(hash) ? view.GetCoins(hash) : outsEmpty;
		outs.ClearUnspendable();

		CCoins outsBlock = CCoins(tx, pindex->nHeight);
		// The CCoins serialization does not serialize negative numbers.
		if (outsBlock.nVersion < 0)
			outs.nVersion = outsBlock.nVersion;
		if (outs != outsBlock)
			fClean = fClean && error("DisconnectBlock() : added transaction mismatch? database corrupted");

		// remove outputs
		outs = CCoins();

		// restore inputs
		if (i > 0) { // not coinbases
			const CTxUndo &txundo = blockUndo.vtxundo[i-1];
			if (txundo.vprevout.size() != tx.vin.size())
				return error("DisconnectBlock() : transaction and undo data inconsistent");
			for (unsigned int j = tx.vin.size(); j-- > 0;)
			{
				const COutPoint &out = tx.vin[j].prevout;
				const CTxInUndo &undo = txundo.vprevout[j];
				CCoins coins;
				view.GetCoins(out.hash, coins); // this can fail if the prevout was already entirely spent
				if (undo.nHeight != 0) {
					// undo data contains height: this is the last output of the prevout tx being spent
					if (!coins.IsPruned())
						fClean = fClean && error("DisconnectBlock() : undo data overwriting existing transaction");
					coins = CCoins();
					coins.fCoinBase = undo.fCoinBase;
					coins.nHeight = undo.nHeight;
					coins.nVersion = undo.nVersion;
				} else {
					if (coins.IsPruned())
						fClean = fClean && error("DisconnectBlock() : undo data adding output to missing transaction");
				}
				if (coins.IsAvailable(out.n))
					fClean = fClean && error("DisconnectBlock() : undo data overwriting existing output");
				if (coins.vout.size() < out.n+1)
					coins.vout.resize(out.n+1);
				coins.vout[out.n] = undo.txout;
				if (!view.SetCoins(out.hash, coins))
					return error("DisconnectBlock() : cannot restore coin inputs");

				// processing inputs for address index
            	const CTxIn input = tx.vin[j];
				const CTxOut &prevout = view.GetOutputFor(tx.vin[j]);
				const CScript &key = prevout.scriptPubKey;

				if (key.IsPayToPublicKeyHash() || key.IsPayToScriptHash()) 
				{
					std::pair<int64_t, int> value;
					
					int64_t nBalance =(view.GetAddressInfo(key,value)) ? value.first + prevout.nValue : prevout.nValue;
					value = std::make_pair(nBalance, pindex->nHeight);
					assert(view.SetAddressInfo(key,value));
					addressInfo[key]=value;
				}
			}
		}
	}

	// move best block pointer to prevout block
	view.SetBestBlock(pindex->pprev->GetBlockHash());

	if (pfClean) { 
		*pfClean = fClean;
		return true;
	}

    if(!RichList.UpdateAddressInfo(addressInfo)) {
    	return state.Abort(_("Failed to update rich list"));
    }

    if(!ServiceList.UpdateServiceInfo(serviceInfo)) {
        return state.Abort(_("Failed to update service list"));
    }

    if(!ServiceItemList.UpdateCouponList(serviceCouponList)) {
        return state.Abort(_("Failed to update coupon list"));
    }

    if(!ServiceItemList.UpdateUbiList(serviceUbiList)) {
        return state.Abort(_("Failed to update ubi recipient list"));
    }

    if(!ServiceItemList.UpdateDexList(serviceDexList)) {
        return state.Abort(_("Failed to update dex list"));
    }

    if(!ServiceItemList.UpdateBookList(serviceBookList)) {
        return state.Abort(_("Failed to update book chapter list"));
    }

    if(!ServiceItemList.UpdateNPList(serviceNPList)) {
        return state.Abort(_("Failed to update np list"));
    }

	return fClean;
}

void static FlushBlockFile(bool fFinalize = false)
{
	LOCK(cs_LastBlockFile);

	CDiskBlockPos posOld(nLastBlockFile, 0);

	FILE *fileOld = OpenBlockFile(posOld);
	if (fileOld) {
		if (fFinalize)
			TruncateFile(fileOld, infoLastBlockFile.nSize);
		FileCommit(fileOld);
		fclose(fileOld);
	}

	fileOld = OpenUndoFile(posOld);
	if (fileOld) {
		if (fFinalize)
			TruncateFile(fileOld, infoLastBlockFile.nUndoSize);
		FileCommit(fileOld);
		fclose(fileOld);
	}
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck() {
	RenameThread("bitcoin-scriptch");
	scriptcheckqueue.Thread();
}

bool ConnectBlock(CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, bool fJustCheck, bool fRichCheck)
{ //TODO: fjarlægja richcheck úr stikum
	AssertLockHeld(cs_main);
	// Check it again in case a previous version let a bad block in
	if (!CheckBlock(block, state, !fJustCheck, !fJustCheck))
		return false;

	// verify that the view's current state corresponds to the previous block
	uint256 hashPrevBlock = pindex->pprev == NULL ? uint256(0) : pindex->pprev->GetBlockHash();
	assert(hashPrevBlock == view.GetBestBlock());

	// Genesis block exception, skipping connection of its transactions(its coinbase is unspendable)
	if (block.GetHash() == Params().HashGenesisBlock()) {
		view.SetBestBlock(pindex->GetBlockHash());
		return true;
	}

	bool fScriptChecks = pindex->nHeight >= Checkpoints::GetTotalBlocksEstimate();
	// Blocks 231253-231311 are inconsistent with the consensus protocol as well as 
	// 289701-289758; 319168-319173; 327532-327536; 334558-334567; 340449-340450; 341431-341434, 342073-342074, 343497-343498
	// 345887-345888; 347035-347036; 355209-355214; 360604-360608; 363847-363848; 367887-367894; 369838-370005;
    fRichCheck = fRichCheck && pindex->nHeight > 370005;

	// Do not allow blocks that contain transactions which 'overwrite' older transactions,
	// unless those are already completely spent.
	// If such overwrites are allowed, coinbases and transactions depending upon those
	// can be duplicated to remove the ability to spend the first instance -- even after
	// being sent to another address.
	// See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
	// This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
	// already refuses previously-known transaction ids entirely.
	for (unsigned int i = 0; i < block.vtx.size(); i++) {
		uint256 hash = block.GetTxHash(i);
		if (view.HaveCoins(hash) && !view.GetCoins(hash).IsPruned())
			return state.DoS(100, error("ConnectBlock() : tried to overwrite transaction"),
					REJECT_INVALID, "bad-txns-BIP30");
	}

	unsigned int flags = SCRIPT_VERIFY_NOCACHE | SCRIPT_VERIFY_P2SH;

	CBlockUndo blockundo;

	CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : NULL);

	int64_t nStart = GetTimeMicros();
	int64_t nFees = 0;
	int nInputs = 0;
	unsigned int nSigOps = 0;
	CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vtx.size()));
	std::vector<std::pair<uint256, CDiskTxPos> > vPos;
	vPos.reserve(block.vtx.size());
	std::map<CScript, std::pair<int64_t, int> > addressInfo;
    std::map<std::string, std::tuple<std::string, std::string, std::string> > serviceInfo;
    std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> > serviceCouponList;
    std::map<std::string, std::tuple<std::string, std::string> > serviceUbiList;
    std::map<std::string, std::tuple<std::string, std::string, std::string> > serviceDexList;
    std::map<std::string, std::tuple<std::string, std::string, std::string> > serviceBookList;
    std::map<std::string, std::tuple<std::string, std::string, std::string> > serviceNPList;

    for (unsigned int i = 0; i < block.vtx.size(); i++)
	{
		const CTransaction &tx = block.vtx[i];

		nInputs += tx.vin.size();
		nSigOps += GetLegacySigOpCount(tx);

		unsigned int maxBlockSize;
		GetMaxBlockSizeByBlock(block,maxBlockSize);

		unsigned int maxBlockSigops=maxBlockSize/50;

		if (nSigOps > maxBlockSigops)
			return state.DoS(100, error("ConnectBlock() : too many sigops"),
					REJECT_INVALID, "bad-blk-sigops");

		if (!tx.IsCoinBase())
		{
			if (!view.HaveInputs(tx))
				return state.DoS(100, error("ConnectBlock() : inputs missing/spent"),
						REJECT_INVALID, "bad-txns-inputs-missingorspent");

			// processing inputs for address index
			for (size_t j = 0; j < tx.vin.size(); j++) 
			{
            	const CTxIn input = tx.vin[j];
				const CTxOut &prevout = view.GetOutputFor(tx.vin[j]);
				const CScript &key = prevout.scriptPubKey;

				if (key.IsPayToPublicKeyHash() || key.IsPayToScriptHash())
				{					
					std::pair<int64_t, int> value;
					if(!view.GetAddressInfo(key,value))
						return state.Abort(_("Failed to get address index"));
					else {
						value = std::make_pair(value.first - prevout.nValue, pindex->nHeight);
						assert(view.SetAddressInfo(key,value));
						addressInfo[key]=value;
					}
				}
			}

			// Add in sigops done by pay-to-script-hash inputs.
			nSigOps += GetP2SHSigOpCount(tx, view);
			if (nSigOps > maxBlockSigops)
				return state.DoS(100, error("ConnectBlock() : too many sigops"),
						REJECT_INVALID, "bad-blk-sigops");
			
			nFees += view.GetValueIn(tx)-tx.GetValueOut();

			std::vector<CScriptCheck> vChecks;
			if (!CheckInputs(tx, state, view, fScriptChecks, flags, nScriptCheckThreads ? &vChecks : NULL))
				return false;
			control.Add(vChecks);
		}

		// processing outputs for address index
		for (unsigned int k = 0; k < tx.vout.size(); k++) {
			const CTxOut &out = tx.vout[k];
			const CScript &key = out.scriptPubKey;
			const int64_t &value = out.nValue;

			if (ContainsOpReturn(tx)) {
			    // Service
                CTxDestination des;
                ExtractDestination(key, des);
                if (CBitcoinAddress(des).ToString() == "B9TRXJzgUJZZ5zPZbywtNfZHeu492WWRxc" && value == 10*COIN) {
                    for (unsigned int l = 0; l < tx.vout.size(); l++) {
                        const CTxOut &outService = tx.vout[l];
                        const CScript &keyService = outService.scriptPubKey;
                        std::string hexString = HexStr(keyService);
                        std::string hexData;
                        std::string newService;
                        std::string serviceName;
                        std::string serviceAddress;
                        std::string serviceType;
                        // Get the op_return hex value
                        if (hexString.substr(0, 2) == "6a") {
                            // Get the op_return value
                            hexData = hexString.substr(4, hexString.size());

                            // New service
                            // Continue if data string begins with "NS"
                            if (hexData.substr(0, 4) == "4e53") {
                                // Retrieve data that follows "NS"
                                newService = hexData.substr(4, hexString.size());
                                std::vector<std::string> strs = splitString(newService, "20");
                                if (strs.size() == 3) {
                                    serviceName = strs.at(0);
                                    serviceAddress = strs.at(1);
                                    serviceType = strs.at(2);
                                    std::string asciiAddress = hexToAscii(serviceAddress);
                                    CBitcoinAddress sAddress = CBitcoinAddress(asciiAddress);

                                    // Check whether address field contains a valid address
                                    if (sAddress.IsValid() && !ServiceList.IsService(asciiAddress) && serviceName.length() <= 50
                                    && (hexToAscii(serviceType) == "1" || hexToAscii(serviceType) == "2" ||
                                    hexToAscii(serviceType) == "3"|| hexToAscii(serviceType) == "4"||
                                    hexToAscii(serviceType) == "5"|| hexToAscii(serviceType) == "6" || hexToAscii(serviceType) == "7"
                                    || hexToAscii(serviceType) == "8")) {
                                        std::tuple<std::string, std::string, std::string> value;
                                        value = std::make_tuple("NS", hexToAscii(serviceName), hexToAscii(serviceType));
                                        assert(view.SetServiceInfo(asciiAddress, value));
                                        serviceInfo[asciiAddress] = value;

                                        if (pwalletMain)
                                        {
                                            pwalletMain->NotifyServicePageChanged(pwalletMain, hexToAscii(serviceName),
                                                    hexToAscii(serviceAddress),
                                                    hexToAscii(serviceType), CT_NEW);
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    std::string toAddress = CBitcoinAddress(des).ToString();
                    if (CBitcoinAddress(toAddress).IsValid()) { // vantar && addressan er a service listanum
                        for (unsigned int l = 0; l < tx.vout.size(); l++) {
                            const CTxOut &outService = tx.vout[l];
                            const CScript &keyService = outService.scriptPubKey;
                            std::string hexString = HexStr(keyService);

                            // If the scriptPubKey contains OP_RETURN(6a)
							if (hexString.substr(0, 2) == "6a") {
								std::string hexData = hexString.substr(4, hexString.size());
                                // If the string starts with "NT" (new coupon)
								if (hexData.substr(0, 4) == "4e54") {
                                    std::string newCoupon = hexData.substr(4, hexString.size());
									std::vector<std::string> strs = splitString(newCoupon, "20");
									// The string has to have 5 params to be valid as a service and saved into the db
									if (strs.size() == 5) {
										std::string couponLocation = strs.at(0);
										std::string couponName = strs.at(1);
										std::string couponDateAndTime = strs.at(2);
										std::string couponValue = strs.at(3);
										std::string couponAddress = strs.at(4);
                                        std::string asciiAddress = hexToAscii(couponAddress);
                                        CBitcoinAddress tAddress = CBitcoinAddress(asciiAddress);
										// If the coupon address is valid then save it to the db
                                        if (tAddress.IsValid()
                                          && is_before(hexToAscii(couponDateAndTime)) 
                                          && couponLocation.length() <= 40 
                                          && couponName.length() <= 40  
                                          && ServiceList.IsService(toAddress)) {
                                            std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> value;
											value = std::make_tuple("NT", toAddress, hexToAscii(couponLocation), hexToAscii(couponName), hexToAscii(couponDateAndTime), hexToAscii(couponValue));
											assert(view.SetCouponList(asciiAddress, value));
											serviceCouponList[asciiAddress]=value;

                                            if (pwalletMain)
                                            {
                                                pwalletMain->NotifyCouponPageChanged(pwalletMain,
                                                        hexToAscii(couponName),
                                                        hexToAscii(couponLocation),
                                                        hexToAscii(couponDateAndTime),
                                                        hexToAscii(couponValue),
                                                        asciiAddress,
                                                        toAddress,
                                                        CT_NEW);
                                            }
                                        }
                                    }
                                }
								// If the string starts with "NU" (new ubi)
                                else if (hexData.substr(0, 4) == "4e55") {
                                    std::string newUbi = hexData.substr(4, hexString.size());
                                    std::vector<std::string> strs = splitString(newUbi, "20");
                                    // The string has to have 1 params to be valid as a ubi and saved into the db
                                    if (strs.size() == 1) {
                                        std::string ubiAddress = strs.at(0);
                                        std::string asciiAddress = hexToAscii(ubiAddress);
                                        CBitcoinAddress uAddress = CBitcoinAddress(asciiAddress);
                                        if (uAddress.IsValid() && ServiceList.IsService(toAddress)) {
                                            std::tuple<std::string, std::string> value;
                                            value = std::make_tuple("NU", toAddress);
                                            assert(view.SetUbiList(asciiAddress, value));
                                            serviceUbiList[asciiAddress]=value;
                                        }
                                    }
                                }
                                // If the string starts with "ND" (new dex)
                                else if (hexData.substr(0, 4) == "4e44") {
                                    std::string newDex = hexData.substr(4, hexString.size());
                                    std::vector<std::string> strs = splitString(newDex, "20");
                                    // The string has to have 2 params to be valid as a dex and saved into the db
                                    if (strs.size() == 2) {
                                        std::string dexAddress = strs.at(0);
                                        std::string dexDescription = strs.at(1);
                                        std::string asciiAddress = hexToAscii(dexAddress);
                                        CBitcoinAddress dAddress = CBitcoinAddress(asciiAddress);
                                        if (dAddress.IsValid() && dexDescription.length() <= 60 && ServiceList.IsService(toAddress)) {
                                            std::tuple<std::string, std::string, std::string> value;
                                            value = std::make_tuple("ND", toAddress, hexToAscii(dexDescription));
                                            assert(view.SetDexList(asciiAddress, value));
                                            serviceDexList[asciiAddress]=value;
                                        }
                                    }
                                }
                                // If the string starts with "NN" (new np)
                                else if (hexData.substr(0, 4) == "4e4e") {
                                    std::string newNP = hexData.substr(4, hexString.size());
                                    std::vector<std::string> strs = splitString(newNP, "20");
                                    // The string has to have 2 params to be valid as a dex and saved into the db
                                    if (strs.size() == 2) {
                                        std::string npAddress = strs.at(0);
                                        std::string npName = strs.at(1);
                                        std::string asciiAddress = hexToAscii(npAddress);
                                        CBitcoinAddress nAddress = CBitcoinAddress(asciiAddress);
                                        if (nAddress.IsValid() && npName.length() <= 60 && ServiceList.IsService(toAddress)) {
                                            std::tuple<std::string, std::string, std::string> value;
                                            value = std::make_tuple("NN", toAddress, hexToAscii(npName));
                                            assert(view.SetNPList(asciiAddress, value));
                                            serviceNPList[asciiAddress]=value;
                                        }
                                    }
                                }
                                // if starts with "DN "
                                else if (hexData.substr(0, 6) == "444e20") {
									std::string newDex = hexData.substr(6, hexString.size());
									std::vector<std::string> strs = splitString(newDex, "20");
									// The string has to have 2 params to be valid as a np and saved into the db
									if (strs.size() == 1) {
										std::string npAddress = strs.at(0);
                                        std::string asciiAddress = hexToAscii(npAddress);
                                        CBitcoinAddress nAddress = CBitcoinAddress(asciiAddress);
										if (nAddress.IsValid() && ServiceList.IsService(toAddress)) {
											std::tuple<std::string, std::string, std::string> value;
											if (!view.GetNPList(asciiAddress, value)) {
												LogPrintf("Failed to read np index");
                                                continue;
                                            } else {
												value = std::make_tuple("DN", asciiAddress, "");
												//assert(view.SetNPList(asciiAddress, value));
												serviceNPList[asciiAddress]=value;
                                                if (pwalletMain) {
                                                    pwalletMain->NotifyServicePageChanged(pwalletMain, "", asciiAddress,
                                                            "", CT_DELETED);
                                                }
											}
										}
									}
                                }
                                // If the string starts with "NB" (new book chapter)
                                else if (hexData.substr(0, 4) == "4e42") {
                                    std::string newBookChapter = hexData.substr(4, hexString.size());
                                    std::vector<std::string> strs = splitString(newBookChapter, "20");
                                    // The string has to have 2 params to be valid as a book chapter and saved into the db
                                    if (strs.size() == 2) {
                                        std::string chapterNum = strs.at(0);
                                        std::string chapterAddress = strs.at(1);

                                        std::string asciiAddress = hexToAscii(chapterAddress);
                                        CBitcoinAddress bAddress = CBitcoinAddress(asciiAddress);
                                        if (bAddress.IsValid() && ServiceList.IsService(toAddress)) {
                                            std::tuple<std::string, std::string, std::string> value;
                                            value = std::make_tuple("NB", toAddress, hexToAscii(chapterNum));
                                            assert(view.SetBookList(asciiAddress, value));
                                            serviceBookList[asciiAddress]=value;
                                        }
                                    }
                                }
                                // If op_return begins with "DS"
                                else if (hexData.substr(0, 6) == "445320") {
                                    std::string serviceAddress = hexData.substr(6, hexString.size());
                                    std::string asciiAddress = hexToAscii(serviceAddress);
                                    CBitcoinAddress sAddress = CBitcoinAddress(asciiAddress);
                                    if (sAddress.IsValid() && ServiceList.IsService(asciiAddress) && ServiceList.IsService(toAddress)) {
                                        std::tuple<std::string, std::string, std::string> value;
                                        value = std::make_tuple("DS", "", "");
                                        assert(view.SetServiceInfo(asciiAddress, value));
                                        serviceInfo[asciiAddress] = value;

                                        if (pwalletMain) {
                                            pwalletMain->NotifyServicePageChanged(pwalletMain, "", asciiAddress,
                                                    "", CT_DELETED);
                                        }
                                    }
                                }
                                // If op_return begins with "DT"
                                else if (hexData.substr(0, 6) == "445420") {
                                    std::string couponAddress = hexData.substr(6, hexString.size());
                                    std::string asciiAddress = hexToAscii(couponAddress);
                                    CBitcoinAddress tAddress = CBitcoinAddress(asciiAddress);
                                    // If the coupon address is valid then save it to the db
                                    if (tAddress.IsValid() && ServiceItemList.IsCoupon(asciiAddress)) {
                                        std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> value;
                                        value = std::make_tuple("DT", toAddress, "", "", "", "");
                                        assert(view.SetCouponList(asciiAddress, value));
                                        serviceCouponList[asciiAddress]=value;

                                        if (pwalletMain)
                                        {
                                            pwalletMain->NotifyCouponPageChanged(pwalletMain, "", "", "", "",
                                                    asciiAddress,
                                                    toAddress,
                                                    CT_DELETED);
                                        }
                                    }
                                }
                                // delete ubi
                                else if (hexData.substr(0, 6) == "445520") {
                                    std::string uAddress = hexData.substr(6, hexString.size());
                                    std::string asciiAddress = hexToAscii(uAddress);
                                    CBitcoinAddress ubiAddress = CBitcoinAddress(asciiAddress);
                                    // If the coupon address is valid then save it to the db
                                    if (ubiAddress.IsValid() && ServiceItemList.IsUbi(asciiAddress)) {
                                        std::tuple<std::string, std::string> value;
                                        value = std::make_tuple("DU", toAddress);
                                        serviceUbiList[asciiAddress] = value;

                                        if (pwalletMain)
                                        {
                                            pwalletMain->NotifyServicePageChanged(pwalletMain, "", asciiAddress,
                                                    "", CT_DELETED);
                                        }
                                    }
                                }
                                // If op_return begins with "BT"
                                else if (hexData.substr(0, 4) == "4254") {
                                    std::string buyCoupon = hexData.substr(4, hexString.size());
                                    std::vector<std::string> strs = splitString(buyCoupon, "20");
                                    // The string has to have 5 params to be valid as a service and saved into the db
                                    if (strs.size() == 5) {
                                        std::string couponLocation = strs.at(0);
                                        std::string couponName = strs.at(1);
                                        std::string couponDateAndTime = strs.at(2);
                                        std::string couponValue = strs.at(3);
                                        std::string couponAddress = strs.at(4);

                                        std::string asciiAddress = hexToAscii(couponAddress);
                                        CBitcoinAddress tAddress = CBitcoinAddress(asciiAddress);
                                        // If the coupon address is valid then save it to the db
                                        if (tAddress.IsValid() && ServiceItemList.IsCoupon(asciiAddress)) {
                                            std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> value;
                                            value = std::make_tuple("BT", toAddress, hexToAscii(couponLocation), hexToAscii(couponName), hexToAscii(couponDateAndTime), hexToAscii(couponValue));
                                            assert(view.SetCouponList(asciiAddress, value));
                                            serviceCouponList[asciiAddress]=value;

                                            if (pwalletMain)
                                            {
                                                pwalletMain->NotifyCouponPageChanged(pwalletMain,
                                                        hexToAscii(couponName),
                                                        hexToAscii(couponLocation),
                                                        hexToAscii(couponDateAndTime),
                                                        hexToAscii(couponValue),
                                                        asciiAddress,
                                                        toAddress,
                                                        CT_DELETED);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (key.IsPayToScriptHash() || key.IsPayToPublicKeyHash())
			{					
				std::pair<int64_t, int> value;
				if(!view.GetAddressInfo(key,value))
					value = std::make_pair(out.nValue, pindex->nHeight);
				else {
					int64_t nBalance = value.first + out.nValue;
					int nHeight = (pindex->nHeight < nRichForkV2Height || tx.IsCoinBase() || (nBalance >= RICH_AMOUNT && value.first < RICH_AMOUNT)) ? pindex -> nHeight : value.second;
					value = std::make_pair(nBalance,nHeight);
				}
				assert(view.SetAddressInfo(key,value));
				addressInfo[key]=value;	
			} 
		}

		CTxUndo txundo;
		UpdateCoins(tx, state, view, txundo, pindex->nHeight, block.GetTxHash(i));
		if (!tx.IsCoinBase())
			blockundo.vtxundo.push_back(txundo);

		vPos.push_back(std::make_pair(block.GetTxHash(i), pos));
		pos.nTxOffset += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);
	}     
    
	int64_t nTime = GetTimeMicros() - nStart;
	if (fBenchmark)
		LogPrintf("- Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin)\n", (unsigned)block.vtx.size(), 0.001 * nTime, 0.001 * nTime / block.vtx.size(), nInputs <= 1 ? 0 : 0.001 * nTime / (nInputs-1));

	if (block.vtx[0].GetValueOut() > GetBlockValue(pindex->nHeight, nFees) + 2*GetBlockValueDividends(pindex->nHeight))
		return state.DoS(100,
				error("ConnectBlock() : coinbase pays too much (actual=%d vs limit=%d)",
						block.vtx[0].GetValueOut(), GetBlockValue(pindex->nHeight, nFees)),
						REJECT_INVALID, "bad-cb-amount"); 
    
    // The coinbase tx must be split: 10% to the miner, 45% to the correct rich address and 45% to one of the EIAS addresses
    if(pindex != NULL && pindex->nHeight >= nRichForkHeight)
    {
    	bool fRichPayment = !fRichCheck;
    	bool fEIASPayment = false;
    	int64_t nValueDividends = GetBlockValueDividends(pindex -> nHeight);
    	CScript richScriptPubKey;
    	CScript EIASScriptPubKey = RichList.NextEIASScriptPubKey(pindex->pprev->nHeight);
    	if(!RichList.NextRichScriptPubKey(richScriptPubKey))
    		richScriptPubKey = RichList.NextEIASScriptPubKey(pindex->pprev->nHeight + 1);

    	BOOST_FOREACH(const CTxOut &txout, block.vtx[0].vout)
    	{
    		if(txout.nValue == nValueDividends)
    		{	// more than one payment of any type would result in the coinbase paying too much
    			if(txout.scriptPubKey == EIASScriptPubKey)
    				fEIASPayment = true;
    			else if(txout.scriptPubKey == richScriptPubKey)
    				fRichPayment = true;
    		}

    		if(fRichPayment && fEIASPayment)
    			break;	
    	}

        if (!fEIASPayment)
        {
            return state.DoS(100, error("ConnectBlock() : EIAS address not correct"), REJECT_INVALID, "bad-eias-payment");
        }     
        if(!fRichPayment)
        {
        	if(!RichList.IsForked())
        		return state.DoS(100, error("ConnectBlock() : rich address not getting paid correctly"), REJECT_INVALID, "bad-rich-payment");
        	else
        		return state.Abort(_("Rich list may be compromised"));
        }

        nTime = GetTimeMicros() - nStart;
    	if (fBenchmark)
			LogPrintf("- Connect EIAS/Rich: %.2fms (fRichCheck: %s)\n", 0.001 * nTime, fRichCheck);
    }
    
	if (!control.Wait())
		return state.DoS(100, false);
	int64_t nTime2 = GetTimeMicros() - nStart;
	if (fBenchmark)
		LogPrintf("- Verify %u txins: %.2fms (%.3fms/txin)\n", nInputs - 1, 0.001 * nTime2, nInputs <= 1 ? 0 : 0.001 * nTime2 / (nInputs-1));

	if (fJustCheck)
		return true;

	// Write undo information to disk
	if (pindex->GetUndoPos().IsNull() || (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS)
	{
		if (pindex->GetUndoPos().IsNull()) {
			CDiskBlockPos pos;
			if (!FindUndoPos(state, pindex->nFile, pos, ::GetSerializeSize(blockundo, SER_DISK, CLIENT_VERSION) + 40))
				return error("ConnectBlock() : FindUndoPos failed");
			if (!blockundo.WriteToDisk(pos, pindex->pprev->GetBlockHash()))
				return state.Abort(_("Failed to write undo data"));

			// update nUndoPos in block index
			pindex->nUndoPos = pos.nPos;
			pindex->nStatus |= BLOCK_HAVE_UNDO;
		}

		pindex->nStatus = (pindex->nStatus & ~BLOCK_VALID_MASK) | BLOCK_VALID_SCRIPTS;

		CDiskBlockIndex blockindex(pindex);
		if (!pblocktree->WriteBlockIndex(blockindex))
			return state.Abort(_("Failed to write block index"));
	}

	if (fTxIndex) {
		if (!pblocktree->WriteTxIndex(vPos))
			return state.Abort(_("Failed to write transaction index"));
	}
	// updated after the rich payment check
    if(!RichList.UpdateAddressInfo(addressInfo)) {
    	return state.Abort(_("Failed to update rich list"));
    }

    if(!ServiceList.UpdateServiceInfo(serviceInfo)) {
        return state.Abort(_("Failed to update service list"));
    }

    if(!ServiceItemList.UpdateCouponList(serviceCouponList)) {
        return state.Abort(_("Failed to update coupon list"));
    }

    if(!ServiceItemList.UpdateUbiList(serviceUbiList)) {
        return state.Abort(_("Failed to update ubi recipient list"));
    }

    if(!ServiceItemList.UpdateDexList(serviceDexList)) {
        return state.Abort(_("Failed to update dex list"));
    }

    if(!ServiceItemList.UpdateBookList(serviceBookList)) {
        return state.Abort(_("Failed to update book chapter list"));
    }

    if(!ServiceItemList.UpdateNPList(serviceNPList)) {
        return state.Abort(_("Failed to update np list"));
    }

	// add this block to the view's block chain
	bool ret;
	ret = view.SetBestBlock(pindex->GetBlockHash());
	assert(ret);

	// Watch for transactions paying to me
	for (unsigned int i = 0; i < block.vtx.size(); i++)
		g_signals.SyncTransaction(block.GetTxHash(i), block.vtx[i], &block);

	return true;
}

// Update the on-disk chain state.
bool static WriteChainState(CValidationState &state) {
	static int64_t nLastWrite = 0;
	if (!IsInitialBlockDownload() || pcoinsTip->GetCacheSize() > nCoinCacheSize || GetTimeMicros() > nLastWrite + 600*1000000) {
		// Typical CCoins structures on disk are around 100 bytes in size.
		// TODO: this does not account for address info
		if (!CheckDiskSpace(100 * 2 * 2 * pcoinsTip->GetCacheSize()))
			return state.Error("out of disk space");
		FlushBlockFile();
		pblocktree->Sync();
		if (!pcoinsTip->Flush())
			return state.Abort(_("Failed to write to coin database"));
		pblocktree->WriteRichListFork(RichList.IsForked());
		pblocktree->WriteServiceListFork(ServiceList.IsForked());
		pblocktree->WriteServiceCouponListFork(ServiceItemList.IsForked());
		pblocktree->WriteServiceUbiListFork(ServiceItemList.IsForked());
		pblocktree->WriteServiceDexListFork(ServiceItemList.IsForked());
		pblocktree->WriteServiceBookListFork(ServiceItemList.IsForked());
        nLastWrite = GetTimeMicros();
	}
	return true;
}

// Update chainActive and related internal data structures.
void static UpdateTip(CBlockIndex *pindexNew) {
	chainActive.SetTip(pindexNew);

	// Update best block in wallet (so we can detect restored wallets)
	bool fIsInitialDownload = IsInitialBlockDownload();
	if ((chainActive.Height() % 20160) == 0 || (!fIsInitialDownload && (chainActive.Height() % 144) == 0))
		g_signals.SetBestChain(chainActive.GetLocator());

	// New best block
	nTimeBestReceived = GetTime();
	mempool.AddTransactionsUpdated(1);
	LogPrintf("UpdateTip: new best=%s  height=%d  log2_work=%.8g  tx=%lu algo=%u  date=%s progress=%f\n",
			chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(), log(chainActive.Tip()->nChainWork.getdouble())/log(2.0), (unsigned long)chainActive.Tip()->nChainTx,
			chainActive.Tip()->GetAlgo(),
			DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
			Checkpoints::GuessVerificationProgress(chainActive.Tip()));

}

// Disconnect chainActive's tip.
bool static DisconnectTip(CValidationState &state)
{
    CBlockIndex *pindexDelete = chainActive.Tip();
	assert(pindexDelete);
	mempool.check(pcoinsTip);
	// Read block from disk.
	CBlock block;
	if (!ReadBlockFromDisk(block, pindexDelete))
		return state.Abort(_("Failed to read block"));
	// Apply the block atomically to the chain state.
	int64_t nStart = GetTimeMicros();
	{
		CCoinsViewCache view(*pcoinsTip, true);
		if (!DisconnectBlock(block, state, pindexDelete, view))
			return error("DisconnectTip() : DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
		assert(view.Flush());
	}
	if (fBenchmark)
		LogPrintf("- Disconnect block: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
	// Write the chain state to disk, if necessary.
	if (!WriteChainState(state))
		return false;
	// Resurrect mempool transactions from the disconnected block.
	BOOST_FOREACH(const CTransaction &tx, block.vtx) {
		// ignore validation errors in resurrected transactions
		list<CTransaction> removed;
		CValidationState stateDummy;
		if (!tx.IsCoinBase())
			if (!AcceptToMemoryPool(mempool, stateDummy, tx, false, NULL))
				mempool.remove(tx, removed, true);
	} 
	mempool.check(pcoinsTip);
	// Update chainActive and related variables.
	UpdateTip(pindexDelete->pprev);
	// Let wallets know transactions went from 1-confirmed to 0-confirmed or conflicted:
	BOOST_FOREACH(const CTransaction &tx, block.vtx)
    {
		SyncWithWallets(tx.GetHash(), tx, NULL);
    }
	return true;
}

// Connect a new block to chainActive.
bool static ConnectTip(CValidationState &state, CBlockIndex *pindexNew)
{    
    assert(pindexNew->pprev == chainActive.Tip());
	mempool.check(pcoinsTip);
	// Read block from disk.
	CBlock block;
	if (!ReadBlockFromDisk(block, pindexNew))
		return state.Abort(_("Failed to read block"));
	// Apply the block atomically to the chain state.
	int64_t nStart = GetTimeMicros();
	{
		CCoinsViewCache view(*pcoinsTip, true);
		CInv inv(MSG_BLOCK, pindexNew->GetBlockHash());
		if (!ConnectBlock(block, state, pindexNew, view, false, true)) {
			if (state.IsInvalid())
				InvalidBlockFound(pindexNew, state);
			return error("ConnectTip() : ConnectBlock %s failed", pindexNew->GetBlockHash().ToString());
		}
		mapBlockSource.erase(inv.hash);
		assert(view.Flush());
	}
	if (fBenchmark)
		LogPrintf("- Connect: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
	// Write the chain state to disk, if necessary.
	if (!WriteChainState(state))
		return false;
	if (fBenchmark)
		LogPrintf("- Write chainstate: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
	// Remove conflicting transactions from the mempool.
	list<CTransaction> txConflicted;
	BOOST_FOREACH(const CTransaction &tx, block.vtx) {
		list<CTransaction> unused;
		mempool.remove(tx, unused);
		mempool.removeConflicts(tx, txConflicted);
	}
	mempool.check(pcoinsTip);
	// Update chainActive & related variables.
	UpdateTip(pindexNew);
	// Tell wallet about transactions that went from mempool
	// to conflicted:
	BOOST_FOREACH(const CTransaction &tx, txConflicted) {
		SyncWithWallets(tx.GetHash(), tx, NULL);
	}
	// ... and about transactions that got confirmed:
	BOOST_FOREACH(const CTransaction &tx, block.vtx)
    {
		SyncWithWallets(tx.GetHash(), tx, &block);
    }
    return true;
}

// Make chainMostWork correspond to the chain with the most work in it, that isn't known to be invalid
void static FindMostWorkChain() {
	CBlockIndex *pindexNew = NULL;

	// In case the current best is invalid, do not consider it.
	while (chainMostWork.Tip() && (chainMostWork.Tip()->nStatus & BLOCK_FAILED_MASK)) {
		setBlockIndexValid.erase(chainMostWork.Tip());
		chainMostWork.SetTip(chainMostWork.Tip()->pprev);
	}

	do {
		// Find the best candidate header.
		{
			std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexValid.rbegin();
			if (it == setBlockIndexValid.rend())
				return;
			pindexNew = *it;
		}

		// Check whether all blocks on the path between the currently active chain and the candidate are valid.
		CBlockIndex *pindexTest = pindexNew;
		bool fInvalidAncestor = false;
		while (pindexTest && !chainActive.Contains(pindexTest)) {
			if (pindexTest->nStatus & BLOCK_FAILED_MASK) {
				// Candidate has an invalid ancestor, remove entire chain from the set.
				if (pindexBestInvalid == NULL || pindexNew->nChainWork > pindexBestInvalid->nChainWork)
					pindexBestInvalid = pindexNew;
				CBlockIndex *pindexFailed = pindexNew;
				while (pindexTest != pindexFailed) {
					pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
					setBlockIndexValid.erase(pindexFailed);
					pindexFailed = pindexFailed->pprev;
				}
				fInvalidAncestor = true;
				break;
			}
			pindexTest = pindexTest->pprev;
		}
		if (fInvalidAncestor)
			continue;

		break;
	} while(true);

	// Check whether it's actually an improvement.
	if (chainMostWork.Tip() && !CBlockIndexWorkComparator()(chainMostWork.Tip(), pindexNew))
		return;

	// We have a new best.
	chainMostWork.SetTip(pindexNew);

}

// Try to activate to the most-work chain (thereby connecting it).

bool ActivateBestChain(CValidationState &state) {
	LOCK(cs_main);
	CBlockIndex *pindexOldTip = chainActive.Tip();
	bool fComplete = false;
	while (!fComplete) {
		FindMostWorkChain();
		fComplete = true;
		if(chainActive.Height() >= Checkpoints::GetTotalBlocksEstimate() && chainActive.Tip())
			if(!chainMostWork.Contains(chainActive.Tip())) {
                RichList.SetForked(true);
                ServiceList.SetForked(true);
                ServiceItemList.SetForked(true);
            }

		// Check whether we have something to do.
		if (chainMostWork.Tip() == NULL) break;	

		// Disconnect active blocks which are no longer in the best chain.
		while (chainActive.Tip() && !chainMostWork.Contains(chainActive.Tip())) {
			if (!DisconnectTip(state))
				return false;
		}
 		
 		// heights of rich addresses need to be rolled back before new blocks are connected
		if(!RichList.UpdateRichAddressHeights())
			return false; //TODO: láta vita?
		else {
			RichList.SetForked(false);
		}

        if(!ServiceList.UpdateServiceAddressHeights())
            return false; //TODO: láta vita?
        else {
            ServiceList.SetForked(false);
        }

        if(!ServiceItemList.UpdateCouponListHeights())
            return false; //TODO: láta vita?
        else {
            ServiceItemList.SetForked(false);
        }

        if(!ServiceItemList.UpdateUbiListHeights())
            return false; //TODO: láta vita?
        else {
            ServiceItemList.SetForked(false);
        }

        if(!ServiceItemList.UpdateDexListHeights())
            return false; //TODO: láta vita?
        else {
            ServiceItemList.SetForked(false);
        }

        if(!ServiceItemList.UpdateBookListHeights())
            return false; //TODO: láta vita?
        else {
            ServiceItemList.SetForked(false);
        }

		// Connect new blocks.
		while (!chainActive.Contains(chainMostWork.Tip())) {
			CBlockIndex *pindexConnect = chainMostWork[chainActive.Height() + 1];
			if (!ConnectTip(state, pindexConnect)) {
				if (state.IsInvalid()) {
					// The block violates a consensus rule.
					if (!state.CorruptionPossible())
						InvalidChainFound(chainMostWork.Tip());
					fComplete = false;
					state = CValidationState();
					break;
				} else {
					// A system error occurred (disk space, database error, ...).
					return false;
				}
			}
		}
	}

	if (chainActive.Tip() != pindexOldTip) {
		std::string strCmd = GetArg("-blocknotify", "");
		if (!IsInitialBlockDownload() && !strCmd.empty())
		{
			boost::replace_all(strCmd, "%s", chainActive.Tip()->GetBlockHash().GetHex());
			boost::thread t(runCommand, strCmd); // thread runs free
		}
	}

	return true;
}

bool AddToBlockIndex(CBlock& block, CValidationState& state, const CDiskBlockPos& pos)
{
	// Check for duplicate
	uint256 hash = block.GetHash();
	if (mapBlockIndex.count(hash))
		return state.Invalid(error("AddToBlockIndex() : %s already exists", hash.ToString()), 0, "duplicate");

	// Construct new block index object
	CBlockIndex* pindexNew = new CBlockIndex(block);
	assert(pindexNew);
	{
		LOCK(cs_nBlockSequenceId);
		pindexNew->nSequenceId = nBlockSequenceId++;
	}
	map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
	pindexNew->phashBlock = &((*mi).first);
	map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
	if (miPrev != mapBlockIndex.end())
	{
		pindexNew->pprev = (*miPrev).second;
		pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
	}
	pindexNew->nTx = block.vtx.size();
	pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + pindexNew->GetBlockWorkAdjusted().getuint256();
	pindexNew->nChainTx = (pindexNew->pprev ? pindexNew->pprev->nChainTx : 0) + pindexNew->nTx;
	pindexNew->nFile = pos.nFile;
	pindexNew->nDataPos = pos.nPos;
	pindexNew->nUndoPos = 0;
	pindexNew->nStatus = BLOCK_VALID_TRANSACTIONS | BLOCK_HAVE_DATA;
	setBlockIndexValid.insert(pindexNew);

	if (!pblocktree->WriteBlockIndex(CDiskBlockIndex(pindexNew)))
		return state.Abort(_("Failed to write block index"));

	// New best?
	if (!ActivateBestChain(state))
		return false;

	LOCK(cs_main);
	if (pindexNew == chainActive.Tip())
	{
		// Clear fork warning if its no longer applicable
		CheckForkWarningConditions();
		// Notify UI to display prev block's coinbase if it was ours
		static uint256 hashPrevBestCoinBase;
		g_signals.UpdatedTransaction(hashPrevBestCoinBase);
		hashPrevBestCoinBase = block.GetTxHash(0);
	} 
	else
	{
		CheckForkWarningConditionsOnNewFork(pindexNew);
	}
		

	if (!pblocktree->Flush())
		return state.Abort(_("Failed to sync block index"));

	uiInterface.NotifyBlocksChanged();
	return true;
}


bool FindBlockPos(CValidationState &state, CDiskBlockPos &pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
	bool fUpdatedLast = false;

	LOCK(cs_LastBlockFile);

	if (fKnown) {
		if (nLastBlockFile != pos.nFile) {
			nLastBlockFile = pos.nFile;
			infoLastBlockFile.SetNull();
			pblocktree->ReadBlockFileInfo(nLastBlockFile, infoLastBlockFile);
			fUpdatedLast = true;
		}
	} else {
		while (infoLastBlockFile.nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
			LogPrintf("Leaving block file %i: %s\n", nLastBlockFile, infoLastBlockFile.ToString());
			FlushBlockFile(true);
			nLastBlockFile++;
			infoLastBlockFile.SetNull();
			pblocktree->ReadBlockFileInfo(nLastBlockFile, infoLastBlockFile); // check whether data for the new file somehow already exist; can fail just fine
			fUpdatedLast = true;
		}
		pos.nFile = nLastBlockFile;
		pos.nPos = infoLastBlockFile.nSize;
	}

	infoLastBlockFile.nSize += nAddSize;
	infoLastBlockFile.AddBlock(nHeight, nTime);

	if (!fKnown) {
		unsigned int nOldChunks = (pos.nPos + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
		unsigned int nNewChunks = (infoLastBlockFile.nSize + BLOCKFILE_CHUNK_SIZE - 1) / BLOCKFILE_CHUNK_SIZE;
		if (nNewChunks > nOldChunks) {
			if (CheckDiskSpace(nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos)) {
				FILE *file = OpenBlockFile(pos);
				if (file) {
					LogPrintf("Pre-allocating up to position 0x%x in blk%05u.dat\n", nNewChunks * BLOCKFILE_CHUNK_SIZE, pos.nFile);
					AllocateFileRange(file, pos.nPos, nNewChunks * BLOCKFILE_CHUNK_SIZE - pos.nPos);
					fclose(file);
				}
			}
			else
				return state.Error("out of disk space");
		}
	}

	if (!pblocktree->WriteBlockFileInfo(nLastBlockFile, infoLastBlockFile))
		return state.Abort(_("Failed to write file info"));
	if (fUpdatedLast)
		pblocktree->WriteLastBlockFile(nLastBlockFile);

	return true;
}

bool FindUndoPos(CValidationState &state, int nFile, CDiskBlockPos &pos, unsigned int nAddSize)
{
	pos.nFile = nFile;

	LOCK(cs_LastBlockFile);

	unsigned int nNewSize;
	if (nFile == nLastBlockFile) {
		pos.nPos = infoLastBlockFile.nUndoSize;
		nNewSize = (infoLastBlockFile.nUndoSize += nAddSize);
		if (!pblocktree->WriteBlockFileInfo(nLastBlockFile, infoLastBlockFile))
			return state.Abort(_("Failed to write block info"));
	} else {
		CBlockFileInfo info;
		if (!pblocktree->ReadBlockFileInfo(nFile, info))
			return state.Abort(_("Failed to read block info"));
		pos.nPos = info.nUndoSize;
		nNewSize = (info.nUndoSize += nAddSize);
		if (!pblocktree->WriteBlockFileInfo(nFile, info))
			return state.Abort(_("Failed to write block info"));
	}

	unsigned int nOldChunks = (pos.nPos + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
	unsigned int nNewChunks = (nNewSize + UNDOFILE_CHUNK_SIZE - 1) / UNDOFILE_CHUNK_SIZE;
	if (nNewChunks > nOldChunks) {
		if (CheckDiskSpace(nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos)) {
			FILE *file = OpenUndoFile(pos);
			if (file) {
				LogPrintf("Pre-allocating up to position 0x%x in rev%05u.dat\n", nNewChunks * UNDOFILE_CHUNK_SIZE, pos.nFile);
				AllocateFileRange(file, pos.nPos, nNewChunks * UNDOFILE_CHUNK_SIZE - pos.nPos);
				fclose(file);
			}
		}
		else
			return state.Error("out of disk space");
	}

	return true;
}

void GetMaxBlockSizeByBlock(const CBlock &block,unsigned int &maxBlockSize)
{
	uint256 hash=block.GetHash();
	if(mapBlockIndex.count(hash))
	{
		map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
		CBlockIndex* pindex = (*mi).second;

		if(pindex&&chainActive.Contains(pindex))
		{
			maxBlockSize=GetMaxBlockSize(pindex->nHeight);
			return;
		}
	}
	maxBlockSize=GetMaxBlockSize(chainActive.Height());
}

bool CheckBlock(const CBlock& block, CValidationState& state, bool fCheckPOW, bool fCheckMerkleRoot)
{
	// Initialize required vars for this function
	unsigned int maxBlockSize;
	GetMaxBlockSizeByBlock(block,maxBlockSize);

	// hash variables (note: these are not the same)
	uint256 hash = block.GetHash();
	//uint256 PoWhash = block.GetPoWHash(block.GetAlgo());
	//LogPrintf("CheckBlock(): hash = %s\n", hash.ToString());
	
	//Get prev block index
	CBlockIndex* pindexPrev = NULL;
	int nHeight = 0;
	if (hash != Params().HashGenesisBlock()) {
		map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
		if (mi == mapBlockIndex.end()) {
			// This gets hit while syncing and a new block hits the chain.
			LogPrintf("CheckBlock() : prev block not found for %s. Ignoring...\n", hash.ToString());
			//return state.DoS(10, error("CheckBlock() : prev block not found"), 0, "bad-prevblk");
			}
		else {
			pindexPrev = (*mi).second;
			nHeight = pindexPrev->nHeight+1;
			}
		}

	// Size limits
	if (block.vtx.empty() || block.vtx.size() > maxBlockSize || ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION) > maxBlockSize)
		return state.DoS(100, error("CheckBlock() : size limits failed"),
				REJECT_INVALID, "bad-blk-length");

	if(pindexPrev) {
		unsigned int nCheckBits = 0;
		nCheckBits = GetNextWorkRequired(pindexPrev, &block, block.GetAlgo());
	
		// Is this really needed?
		if (block.nBits != nCheckBits) {
			LogPrintf("CheckBlock() : proof of work in block not the same as calculated at height %i. Ignoring...\n", nHeight);
			//return state.DoS(100, error("CheckBlock() : incorrect proof of work in header"), 
			//		REJECT_INVALID, "bad-diffbits");
			}

		// Check proof of work matches claimed amount
		//if (fCheckPOW && !CheckProofOfWork(block.GetPoWHash(block.GetAlgo()), block.nBits, block.GetAlgo()))
		if (fCheckPOW && !CheckProofOfWork(block.GetPoWHash(block.GetAlgo()), nCheckBits, block.GetAlgo()))
			return state.DoS(50, error("CheckBlock() : proof of work failed"),
					REJECT_INVALID, "high-hash");
		}
		else
		{
		// No previous block (new block while syncing), so we can't calculate the NextWorkRequired.
		// FIXME: In pricipal we should not check it now. Rewrite later.
		if (fCheckPOW && !CheckProofOfWork(block.GetPoWHash(block.GetAlgo()), block.nBits, block.GetAlgo())) {
			//LogPrintf("CheckBlock() : proof of work failed at %i. Ignoring...\n", nHeight);
			return state.DoS(50, error("CheckBlock() : proof of work failed"),
					REJECT_INVALID, "high-hash");
			}
		}

	// Check timestamp
	if (block.GetBlockTime() > GetAdjustedTime() + 2 * 60 * 60)
		return state.Invalid(error("CheckBlock() : block timestamp too far in the future"),
				REJECT_INVALID, "time-too-new");

	// Check amount of algos in row
	if(pindexPrev) {
		// Check count of sequence of same algo
		if ( (TestNet() && (nHeight >= 100)) || (nHeight > (nRichForkHeight + nBlockSequentialAlgoMaxCount)) ) {
			int nAlgo = block.GetAlgo();
			int nAlgoCount = 1;
			CBlockIndex* piPrev = pindexPrev;
			while (piPrev && (nAlgoCount <= nBlockSequentialAlgoMaxCount)) {
				if (piPrev->GetAlgo() != nAlgo)
					break;
				nAlgoCount++;
				piPrev = piPrev->pprev;
				}
				if (nAlgoCount > nBlockSequentialAlgoMaxCount) {
					return state.DoS(100, error("CheckBlock() : too many blocks from same algo"),
									REJECT_INVALID, "algo-toomany");
				}
			}
		}

	// First transaction must be coinbase, the rest must not be
	if (block.vtx.empty() || !block.vtx[0].IsCoinBase())
		return state.DoS(100, error("CheckBlock() : first tx is not coinbase"),
				REJECT_INVALID, "bad-cb-missing");
	for (unsigned int i = 1; i < block.vtx.size(); i++)
		if (block.vtx[i].IsCoinBase())
			return state.DoS(100, error("CheckBlock() : more than one coinbase"),
					REJECT_INVALID, "bad-cb-multiple");

	// Check transactions
	BOOST_FOREACH(const CTransaction& tx, block.vtx)
	if (!CheckTransaction(tx, state))
		return error("CheckBlock() : CheckTransaction failed");

	block.BuildMerkleTree();

	// Check for duplicate txids before ConnectInputs() to prevent DoS attack.
	set<uint256> uniqueTx;
	for (unsigned int i = 0; i < block.vtx.size(); i++) {
		uniqueTx.insert(block.GetTxHash(i));
	}
	if (uniqueTx.size() != block.vtx.size())
		return state.DoS(100, error("CheckBlock() : duplicate transaction"),
				REJECT_INVALID, "bad-txns-duplicate", true);

	unsigned int nSigOps = 0;
	BOOST_FOREACH(const CTransaction& tx, block.vtx)
	{
		nSigOps += GetLegacySigOpCount(tx);
	}

	unsigned int maxBlockSigops=maxBlockSize/50;

	if (nSigOps > maxBlockSigops)
		return state.DoS(100, error("CheckBlock() : out-of-bounds SigOpCount"),
				REJECT_INVALID, "bad-blk-sigops", true);

	// Check merkle root
	if (fCheckMerkleRoot && block.hashMerkleRoot != block.vMerkleTree.back())
		return state.DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"),
				REJECT_INVALID, "bad-txnmrklroot", true);

	//LogPrintf("CheckBlock() finished successful: hash = %s\n", hash.ToString());
	return true;
}

bool AcceptBlock(CBlock& block, CValidationState& state, CDiskBlockPos* dbp)
{
    AssertLockHeld(cs_main);
	// Check for duplicate
	uint256 hash = block.GetHash();
	//LogPrintf("AcceptBlock(): hash = %s\n", hash.ToString());
	if (mapBlockIndex.count(hash))
		return state.Invalid(error("AcceptBlock() : block already in mapBlockIndex"), 0, "duplicate");

	// Get prev block index
	CBlockIndex* pindexPrev = NULL;
	int nHeight = 0;
	if (hash != Params().HashGenesisBlock()) {
		map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
		if (mi == mapBlockIndex.end())
			return state.DoS(10, error("AcceptBlock() : prev block not found"), 0, "bad-prevblk");
		pindexPrev = (*mi).second;
		nHeight = pindexPrev->nHeight+1;

	/** // Check count of sequence of same algo
	if ( (TestNet() && (nHeight >= 100)) || (nHeight > (multiAlgoDiffChangeTarget + nBlockSequentialAlgoMaxCount)) )
	{
			int nAlgo = block.GetAlgo();
			int nAlgoCount = 1;
			CBlockIndex* piPrev = pindexPrev;
			while (piPrev && (nAlgoCount <= nBlockSequentialAlgoMaxCount))
			{
					if (piPrev->GetAlgo() != nAlgo)
							break;
					nAlgoCount++;
					piPrev = piPrev->pprev;
			}
			if (nAlgoCount > nBlockSequentialAlgoMaxCount)
			{
					return state.DoS(100, error("AcceptBlock() : too many blocks from same algo"),
													REJECT_INVALID, "algo-toomany");
		}
	}	**/

		// Check proof of work
		// BioMike: There is a problem here:
		//   A check on hash validation, based on block.Bits has already been performed (See CheckBlock(...)),
		//   yet, now we check here if block.nBits is right... should be other way around.
		//if (block.nBits != GetNextWorkRequired(pindexPrev, &block, block.GetAlgo()))
		//	return state.DoS(100, error("AcceptBlock() : incorrect proof of work"), 
		//				    REJECT_INVALID, "bad-diffbits");

		if ( !TestNet() && nHeight < nRichForkHeight && block.GetAlgo() != ALGO_SCRYPT )
			return state.Invalid(error("AcceptBlock() : incorrect hasing algo, only scrypt accepted until block %d", nRichForkHeight),
					REJECT_INVALID, "bad-hashalgo");

		// Check timestamp against prev
		if (block.GetBlockTime() <= pindexPrev->GetMedianTimePast())
			return state.Invalid(error("AcceptBlock() : block's timestamp is too early"),
					REJECT_INVALID, "time-too-old");

		// Check that all transactions are finalized
		BOOST_FOREACH(const CTransaction& tx, block.vtx)
		if (!IsFinalTx(tx, nHeight, block.GetBlockTime()))
			return state.DoS(10, error("AcceptBlock() : contains a non-final transaction"),
					REJECT_INVALID, "bad-txns-nonfinal");

		// Check that the block chain matches the known block chain up to a checkpoint
		if (!Checkpoints::CheckBlock(nHeight, hash))
			return state.DoS(100, error("AcceptBlock() : rejected by checkpoint lock-in at %d", nHeight),
					REJECT_CHECKPOINT, "checkpoint mismatch");

		// Don't accept any forks from the main chain prior to last checkpoint
		CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
		if (pcheckpoint && nHeight < pcheckpoint->nHeight)
			return state.DoS(100, error("AcceptBlock() : forked chain older than last checkpoint (height %d)", nHeight));

		// Reject block.nVersion=1 blocks when 95% (75% on testnet) of the network has upgraded:
		if (block.nVersion < 2)
		{
			if ((!TestNet() && CBlockIndex::IsSuperMajority(2, pindexPrev, 950, 1000)) ||
					(TestNet() && CBlockIndex::IsSuperMajority(2, pindexPrev, 75, 100)))
			{
				return state.Invalid(error("AcceptBlock() : rejected nVersion=1 block"),
						REJECT_OBSOLETE, "bad-version");
			}
		}
		// Enforce block.nVersion=2 rule that the coinbase starts with serialized block height
		if (block.nVersion >= 2)
		{
			// if 750 of the last 1,000 blocks are version 2 or greater (51/100 if testnet):
			if ((!TestNet() && CBlockIndex::IsSuperMajority(2, pindexPrev, 750, 1000)) ||
					(TestNet() && CBlockIndex::IsSuperMajority(2, pindexPrev, 51, 100)))
			{
				CScript expect = CScript() << nHeight;
				if (block.vtx[0].vin[0].scriptSig.size() < expect.size() ||
						!std::equal(expect.begin(), expect.end(), block.vtx[0].vin[0].scriptSig.begin()))
					return state.DoS(100, error("AcceptBlock() : block height mismatch in coinbase"),
							REJECT_INVALID, "bad-cb-height");
			}
		}
	}

	// Write block to history file
	try {
		unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
		CDiskBlockPos blockPos;
		if (dbp != NULL)
			blockPos = *dbp;
		if (!FindBlockPos(state, blockPos, nBlockSize+8, nHeight, block.nTime, dbp != NULL))
			return error("AcceptBlock() : FindBlockPos failed");
		if (dbp == NULL)
			if (!WriteBlockToDisk(block, blockPos))
				return state.Abort(_("Failed to write block"));
		if (!AddToBlockIndex(block, state, blockPos))
			return error("AcceptBlock() : AddToBlockIndex failed");
	} catch(std::runtime_error &e) {
		return state.Abort(_("System error: ") + e.what());
	}

	// Relay inventory, but don't relay old inventory during initial block download
	int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
	if (chainActive.Tip()->GetBlockHash() == hash)
	{
		LOCK(cs_vNodes);
		BOOST_FOREACH(CNode* pnode, vNodes)
		if (chainActive.Height() > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
			pnode->PushInventory(CInv(MSG_BLOCK, hash));
	}
	//LogPrintf("AcceptBlock() successful: hash = %s\n", hash.ToString());
	return true;
}

bool CBlockIndex::IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
	unsigned int nFound = 0;
	for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++)
	{
		if (pstart->nVersion >= minVersion)
			++nFound;
		pstart = pstart->pprev;
	}
	return (nFound >= nRequired);
}

int64_t CBlockIndex::GetMedianTime() const
{
	AssertLockHeld(cs_main);
	const CBlockIndex* pindex = this;
	for (int i = 0; i < nMedianTimeSpan/2; i++)
	{
		if (!chainActive.Next(pindex))
			return GetBlockTime();
		pindex = chainActive.Next(pindex);
	}
	return pindex->GetMedianTimePast();
}

void PushGetBlocks(CNode* pnode, CBlockIndex* pindexBegin, uint256 hashEnd)
{
	AssertLockHeld(cs_main);
	// Filter out duplicate requests
	if (pindexBegin == pnode->pindexLastGetBlocksBegin && hashEnd == pnode->hashLastGetBlocksEnd)
		return;
	pnode->pindexLastGetBlocksBegin = pindexBegin;
	pnode->hashLastGetBlocksEnd = hashEnd;

	pnode->PushMessage("getblocks", chainActive.GetLocator(pindexBegin), hashEnd);
}

bool ProcessBlock(CValidationState &state, CNode* pfrom, CBlock* pblock, CDiskBlockPos *dbp)
{
	AssertLockHeld(cs_main);
	//LogPrintf("ProcessBlock() started\n");
	// Check for duplicate
	uint256 hash = pblock->GetHash();
	if (mapBlockIndex.count(hash))
		return state.Invalid(error("ProcessBlock() : already have block %d %s", mapBlockIndex[hash]->nHeight, hash.ToString()), 0, "duplicate");
	if (mapOrphanBlocks.count(hash))
		return state.Invalid(error("ProcessBlock() : already have block (orphan) %s", hash.ToString()), 0, "duplicate");

	// Preliminary checks
	if (!CheckBlock(*pblock, state))
		return error("ProcessBlock() : CheckBlock FAILED");

	CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
	if (pcheckpoint && pblock->hashPrevBlock != (chainActive.Tip() ? chainActive.Tip()->GetBlockHash() : uint256(0)))
	{
		if((pblock->GetBlockTime() - pcheckpoint->nTime) < 0) {
			return state.DoS(100, error("ProcessBlock() : block has a time stamp of %u before the last checkpoint of %u", pblock->GetBlockTime(), pcheckpoint->nTime));
         }
	}

	// If we don't already have its previous block, shunt it off to holding area until we get it
	if (pblock->hashPrevBlock != 0 && !mapBlockIndex.count(pblock->hashPrevBlock))
	{
		LogPrintf("ProcessBlock: ORPHAN BLOCK %lu, prev=%s\n", (unsigned long)mapOrphanBlocks.size(), pblock->hashPrevBlock.ToString());

		// Accept orphans as long as there is a node to request its parents from
		if (pfrom) {
			PruneOrphanBlocks();
			COrphanBlock* pblock2 = new COrphanBlock();
			{
				CDataStream ss(SER_DISK, CLIENT_VERSION);
				ss << *pblock;
				pblock2->vchBlock = std::vector<unsigned char>(ss.begin(), ss.end());
			}
			pblock2->hashBlock = hash;
			pblock2->hashPrev = pblock->hashPrevBlock;
			mapOrphanBlocks.insert(make_pair(hash, pblock2));
			mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrev, pblock2));

			// Ask this guy to fill in what we're missing
			PushGetBlocks(pfrom, chainActive.Tip(), GetOrphanRoot(hash));
		}
		return true;
	}

	// Store to disk
	if (!AcceptBlock(*pblock, state, dbp))
		return error("ProcessBlock() : AcceptBlock FAILED");

	// Recursively process any orphan blocks that depended on this one
	vector<uint256> vWorkQueue;
	vWorkQueue.push_back(hash);
	for (unsigned int i = 0; i < vWorkQueue.size(); i++)
	{
		uint256 hashPrev = vWorkQueue[i];
		for (multimap<uint256, COrphanBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
				mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
				++mi)
		{
			CBlock block;
			{
				CDataStream ss(mi->second->vchBlock, SER_DISK, CLIENT_VERSION);
				ss >> block;
			}
			block.BuildMerkleTree();
			// Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan resolution.
			CValidationState stateDummy;
			if (AcceptBlock(block, stateDummy))
				vWorkQueue.push_back(mi->second->hashBlock);
			mapOrphanBlocks.erase(mi->second->hashBlock);
			delete mi->second;
		}
		mapOrphanBlocksByPrev.erase(hashPrev);
	}

	LogPrintf("ProcessBlock: ACCEPTED\n");
	return true;
}

CMerkleBlock::CMerkleBlock(const CBlock& block, CBloomFilter& filter)
{
	header = block.GetBlockHeader();

	vector<bool> vMatch;
	vector<uint256> vHashes;

	vMatch.reserve(block.vtx.size());
	vHashes.reserve(block.vtx.size());

	for (unsigned int i = 0; i < block.vtx.size(); i++)
	{
		uint256 hash = block.vtx[i].GetHash();
		if (filter.IsRelevantAndUpdate(block.vtx[i], hash))
		{
			vMatch.push_back(true);
			vMatchedTxn.push_back(make_pair(i, hash));
		}
		else
			vMatch.push_back(false);
		vHashes.push_back(hash);
	}

	txn = CPartialMerkleTree(vHashes, vMatch, block);
}

uint256 CPartialMerkleTree::CalcHash(int height, unsigned int pos, const std::vector<uint256> &vTxid) {
	if (height == 0) {
		// hash at height 0 is the txids themself
		return vTxid[pos];
	} else {
		// calculate left hash
		uint256 left = CalcHash(height-1, pos*2, vTxid), right;
		// calculate right hash if not beyong the end of the array - copy left hash otherwise1
		if (pos*2+1 < CalcTreeWidth(height-1))
			right = CalcHash(height-1, pos*2+1, vTxid);
		else
			right = left;
		// combine subhashes
		return Hash(BEGIN(left), END(left), BEGIN(right), END(right));
	}
}

void CPartialMerkleTree::TraverseAndBuild(int height, unsigned int pos, const std::vector<uint256> &vTxid, const std::vector<bool> &vMatch) {
	// determine whether this node is the parent of at least one matched txid
	bool fParentOfMatch = false;
	for (unsigned int p = pos << height; p < (pos+1) << height && p < nTransactions; p++)
		fParentOfMatch |= vMatch[p];
	// store as flag bit
	vBits.push_back(fParentOfMatch);
	if (height==0 || !fParentOfMatch) {
		// if at height 0, or nothing interesting below, store hash and stop
		vHash.push_back(CalcHash(height, pos, vTxid));
	} else {
		// otherwise, don't store any hash, but descend into the subtrees
		TraverseAndBuild(height-1, pos*2, vTxid, vMatch);
		if (pos*2+1 < CalcTreeWidth(height-1))
			TraverseAndBuild(height-1, pos*2+1, vTxid, vMatch);
	}
}

uint256 CPartialMerkleTree::TraverseAndExtract(int height, unsigned int pos, unsigned int &nBitsUsed, unsigned int &nHashUsed, std::vector<uint256> &vMatch) {
	if (nBitsUsed >= vBits.size()) {
		// overflowed the bits array - failure
		fBad = true;
		return 0;
	}
	bool fParentOfMatch = vBits[nBitsUsed++];
	if (height==0 || !fParentOfMatch) {
		// if at height 0, or nothing interesting below, use stored hash and do not descend
		if (nHashUsed >= vHash.size()) {
			// overflowed the hash array - failure
			fBad = true;
			return 0;
		}
		const uint256 &hash = vHash[nHashUsed++];
		if (height==0 && fParentOfMatch) // in case of height 0, we have a matched txid
			vMatch.push_back(hash);
		return hash;
	} else {
		// otherwise, descend into the subtrees to extract matched txids and hashes
		uint256 left = TraverseAndExtract(height-1, pos*2, nBitsUsed, nHashUsed, vMatch), right;
		if (pos*2+1 < CalcTreeWidth(height-1))
			right = TraverseAndExtract(height-1, pos*2+1, nBitsUsed, nHashUsed, vMatch);
		else
			right = left;
		// and combine them before returning
		return Hash(BEGIN(left), END(left), BEGIN(right), END(right));
	}
}

CPartialMerkleTree::CPartialMerkleTree(const std::vector<uint256> &vTxid, const std::vector<bool> &vMatch, const CBlock &block) : nTransactions(vTxid.size()), fBad(false) {

	this->block=&block;

	// reset state
	vBits.clear();
	vHash.clear();

	// calculate height of tree
	int nHeight = 0;
	while (CalcTreeWidth(nHeight) > 1)
		nHeight++;

	// traverse the partial tree
	TraverseAndBuild(nHeight, 0, vTxid, vMatch);
}

CPartialMerkleTree::CPartialMerkleTree(const std::vector<uint256> &vTxid, const std::vector<bool> &vMatch) : nTransactions(vTxid.size()), fBad(false) {

	this->block=0;

	// reset state
	vBits.clear();
	vHash.clear();

	// calculate height of tree
	int nHeight = 0;
	while (CalcTreeWidth(nHeight) > 1)
		nHeight++;

	// traverse the partial tree
	TraverseAndBuild(nHeight, 0, vTxid, vMatch);
}

CPartialMerkleTree::CPartialMerkleTree() : nTransactions(0), fBad(true) {}

uint256 CPartialMerkleTree::ExtractMatches(std::vector<uint256> &vMatch) {
	vMatch.clear();
	// An empty set will not work
	if (nTransactions == 0)
		return 0;

	unsigned int maxBlockSize;

	GetMaxBlockSizeByBlock(*block, maxBlockSize);

	// check for excessively high numbers of transactions
	if (nTransactions > maxBlockSize / 60) // 60 is the lower bound for the size of a serialized CTransaction
		return 0;
	// there can never be more hashes provided than one for every txid
	if (vHash.size() > nTransactions)
		return 0;
	// there must be at least one bit per node in the partial tree, and at least one node per hash
	if (vBits.size() < vHash.size())
		return 0;
	// calculate height of tree
	int nHeight = 0;
	while (CalcTreeWidth(nHeight) > 1)
		nHeight++;
	// traverse the partial tree
	unsigned int nBitsUsed = 0, nHashUsed = 0;
	uint256 hashMerkleRoot = TraverseAndExtract(nHeight, 0, nBitsUsed, nHashUsed, vMatch);
	// verify that no problems occured during the tree traversal
	if (fBad)
		return 0;
	// verify that all bits were consumed (except for the padding caused by serializing it as a byte sequence)
	if ((nBitsUsed+7)/8 != (vBits.size()+7)/8)
		return 0;
	// verify that all hashes were consumed
	if (nHashUsed != vHash.size())
		return 0;
	return hashMerkleRoot;
}

bool AbortNode(const std::string &strMessage) {
	strMiscWarning = strMessage;
	LogPrintf("*** %s\n", strMessage);
	uiInterface.ThreadSafeMessageBox(strMessage, "", CClientUIInterface::MSG_ERROR);
	StartShutdown();
	return false;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
	uint64_t nFreeBytesAvailable = filesystem::space(GetDataDir()).available;

	// Check for nMinDiskSpace bytes (currently 50MB)
	if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
		return AbortNode(_("Error: Disk space is low!"));

	return true;
}

FILE* OpenDiskFile(const CDiskBlockPos &pos, const char *prefix, bool fReadOnly)
{
	if (pos.IsNull())
		return NULL;
	boost::filesystem::path path = GetDataDir() / "blocks" / strprintf("%s%05u.dat", prefix, pos.nFile);
	boost::filesystem::create_directories(path.parent_path());
	FILE* file = fopen(path.string().c_str(), "rb+");
	if (!file && !fReadOnly)
		file = fopen(path.string().c_str(), "wb+");
	if (!file) {
		LogPrintf("Unable to open file %s\n", path.string());
		return NULL;
	}
	if (pos.nPos) {
		if (fseek(file, pos.nPos, SEEK_SET)) {
			LogPrintf("Unable to seek to position %u of %s\n", pos.nPos, path.string());
			fclose(file);
			return NULL;
		}
	}
	return file;
}

FILE* OpenBlockFile(const CDiskBlockPos &pos, bool fReadOnly) {
	return OpenDiskFile(pos, "blk", fReadOnly);
}

FILE* OpenUndoFile(const CDiskBlockPos &pos, bool fReadOnly) {
	return OpenDiskFile(pos, "rev", fReadOnly);
}

CBlockIndex * InsertBlockIndex(uint256 hash)
{
	if (hash == 0)
		return NULL;

	// Return existing
	map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hash);
	if (mi != mapBlockIndex.end())
		return (*mi).second;

	// Create new
	CBlockIndex* pindexNew = new CBlockIndex();
	if (!pindexNew)
		throw runtime_error("LoadBlockIndex() : new CBlockIndex failed");
	mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
	pindexNew->phashBlock = &((*mi).first);

	return pindexNew;
}

bool static LoadBlockIndexDB()
	{
	if (!pblocktree->LoadBlockIndexGuts())
		return false;
	boost::this_thread::interruption_point();
    // Calculate nChainWork
	vector<pair<int, CBlockIndex*> > vSortedByHeight;
	vSortedByHeight.reserve(mapBlockIndex.size());
	// The following FOREACH loads entries for ALL blocks in random order...
	BOOST_FOREACH(const PAIRTYPE(uint256, CBlockIndex*)& item, mapBlockIndex)
	{
		CBlockIndex* pindex = item.second;
		vSortedByHeight.push_back(make_pair(pindex->nHeight, pindex));
	}
	// Which are sorted here.
	sort(vSortedByHeight.begin(), vSortedByHeight.end());
	BOOST_FOREACH(const PAIRTYPE(int, CBlockIndex*)& item, vSortedByHeight)
	{
        CBlockIndex* pindex = item.second;
		// Next command is taking most time
		pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + pindex->GetBlockWorkAdjusted().getuint256();
		pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;
		if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS && !(pindex->nStatus & BLOCK_FAILED_MASK))
			setBlockIndexValid.insert(pindex);
		if ((pindex->nStatus & BLOCK_FAILED_MASK) && (!pindexBestInvalid || pindex->nChainWork > pindexBestInvalid->nChainWork))
			pindexBestInvalid = pindex;
	}
	// Load block file info
	pblocktree->ReadLastBlockFile(nLastBlockFile);
	LogPrintf("LoadBlockIndexDB(): last block file = %i\n", nLastBlockFile);
	if (pblocktree->ReadBlockFileInfo(nLastBlockFile, infoLastBlockFile))
		LogPrintf("LoadBlockIndexDB(): last block file info: %s\n", infoLastBlockFile.ToString());

	// Check whether we need to continue reindexing
	bool fReindexing = false;
	pblocktree->ReadReindexing(fReindexing);
	fReindex |= fReindexing;

	// Check whether we have a transaction index
	pblocktree->ReadFlag("txindex", fTxIndex);
	LogPrintf("LoadBlockIndexDB(): transaction index %s\n", fTxIndex ? "enabled" : "disabled");

	// Load pointer to end of best chain
	std::map<uint256, CBlockIndex*>::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
	if (it == mapBlockIndex.end())
		return true;
	chainActive.SetTip(it->second);
	LogPrintf("LoadBlockIndexDB(): hashBestChain=%s height=%d date=%s progress=%f\n",
			chainActive.Tip()->GetBlockHash().ToString(), chainActive.Height(),
			DateTimeStrFormat("%Y-%m-%d %H:%M:%S", chainActive.Tip()->GetBlockTime()),
			Checkpoints::GuessVerificationProgress(chainActive.Tip()));

	return true;
						}

bool VerifyDB(int nCheckLevel, int nCheckDepth)
{
	LOCK(cs_main);
	if (chainActive.Tip() == NULL || chainActive.Tip()->pprev == NULL)
		return true;

	// Verify blocks in the best chain
	if (nCheckDepth <= 0)
		nCheckDepth = 1000000000; // suffices until the year 19000
	if (nCheckDepth > chainActive.Height())
		nCheckDepth = chainActive.Height();
	nCheckLevel = std::max(0, std::min(4, nCheckLevel));
	LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
	CCoinsViewCache coins(*pcoinsTip, true);
	CBlockIndex* pindexState = chainActive.Tip();
    CBlockIndex* pindexFailure = NULL;
    int nGoodTransactions = 0;
    CValidationState state;
	for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev)
	{
        boost::this_thread::interruption_point();
        if (pindex->nHeight < chainActive.Height()-nCheckDepth)
			break;
        CBlock block;
		// check level 0: read from disk
		if (!ReadBlockFromDisk(block, pindex))
			return error("VerifyDB() : *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
        // check level 1: verify block validity
		if (nCheckLevel >= 1 && !CheckBlock(block, state))
			return error("VerifyDB() : *** found bad block at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
		// check level 2: verify undo validity
		if (nCheckLevel >= 2 && pindex) {
			CBlockUndo undo;
			CDiskBlockPos pos = pindex->GetUndoPos();
			if (!pos.IsNull()) {
				if (!undo.ReadFromDisk(pos, pindex->pprev->GetBlockHash()))
					return error("VerifyDB() : *** found bad undo data at %d, hash=%s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
			}
		}
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
		if (nCheckLevel >= 3 && pindex == pindexState && (coins.GetCacheSize() + pcoinsTip->GetCacheSize()) <= 2*nCoinCacheSize + 32000) {
            bool fClean = true;
            if (!DisconnectBlock(block, state, pindex, coins, &fClean))
                return error("VerifyDB() : *** irrecoverable inconsistency in block data at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
            pindexState = pindex->pprev;
            if (!fClean) {
                nGoodTransactions = 0;
				pindexFailure = pindex;
            } else
                nGoodTransactions += block.vtx.size();
        }
	}
    if (pindexFailure)
		return error("VerifyDB() : *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", chainActive.Height() - pindexFailure->nHeight + 1, nGoodTransactions);

	// check level 4: try reconnecting blocks
	if (nCheckLevel >= 4) {
		CBlockIndex *pindex = pindexState;
		while (pindex != chainActive.Tip()) {
			boost::this_thread::interruption_point();
			pindex = chainActive.Next(pindex);
			CBlock block;
			if (!ReadBlockFromDisk(block, pindex))
				return error("VerifyDB() : *** ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
			if (!ConnectBlock(block, state, pindex, coins, false, false))
				return error("VerifyDB() : *** found unconnectable block at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
		}
	}

    LogPrintf("No coin database inconsistencies in last %i blocks (%i transactions)\n", chainActive.Height() - pindexState->nHeight, nGoodTransactions);

	return true;
}

void UnloadBlockIndex()
{
	mapBlockIndex.clear();
	setBlockIndexValid.clear();
	chainActive.SetTip(NULL);
	pindexBestInvalid = NULL;
}

bool LoadBlockIndex()
{
	// Load block index from databases
	if (!fReindex && !LoadBlockIndexDB())
		return false;
	return true;
}

bool InitBlockIndex() {
	LOCK(cs_main);
	// Check whether we're already initialized
	if (chainActive.Genesis() != NULL)
		return true;

	// Use the provided setting for -txindex in the new database
	fTxIndex = GetBoolArg("-txindex", false);
	pblocktree->WriteFlag("txindex", fTxIndex);
	LogPrintf("Initializing databases...\n");

	// Only add the genesis block if not reindexing (in which case we reuse the one already on disk)
	if (!fReindex) {
		try {
			CBlock &block = const_cast<CBlock&>(Params().GenesisBlock());
			// Start new block file
			unsigned int nBlockSize = ::GetSerializeSize(block, SER_DISK, CLIENT_VERSION);
			CDiskBlockPos blockPos;
			CValidationState state;
			if (!FindBlockPos(state, blockPos, nBlockSize+8, 0, block.nTime))
				return error("LoadBlockIndex() : FindBlockPos failed");
			if (!WriteBlockToDisk(block, blockPos))
				return error("LoadBlockIndex() : writing genesis block to disk failed");
			if (!AddToBlockIndex(block, state, blockPos))
				return error("LoadBlockIndex() : genesis block not accepted");
		} catch(std::runtime_error &e) {
			return error("LoadBlockIndex() : failed to initialize block database: %s", e.what());
		}
	}

	return true;
}

bool InitRichList(CCoinsView &dbview)
{
	LOCK(cs_main);
	if (fReindex || chainActive.Genesis() == NULL) {
		std::vector<unsigned char> v;
    	v.assign(21,'0');
    	if(!dbview.SetAddressInfo(CScript(v),std::make_pair(1,0))) {
    		return false; }
		pblocktree->WriteFlag("addressinfo", true);
	}
	bool dummy;
	if(!pblocktree->ReadFlag("addressinfo", dummy))
		return false;
	return true;

}

bool InitServiceList(CCoinsView &dbview)
{
    LOCK(cs_main);
    if (fReindex || chainActive.Genesis() == NULL) {
        if(!dbview.SetServiceInfo(string("service"), std::make_tuple("1", "0", "0"))) {
            return false; }
        pblocktree->WriteFlag("serviceinfo", true);
    }
    bool dummy;
    if(!pblocktree->ReadFlag("serviceinfo", dummy))
        return false;
    return true;

}

bool InitServiceCouponList(CCoinsView &dbview)
{
    LOCK(cs_main);
    if (fReindex || chainActive.Genesis() == NULL) {
        if(!dbview.SetCouponList(string("coupon"),std::make_tuple("1", "0", "0", "0", "0", "0"))) {
            return false; }
        pblocktree->WriteFlag("couponlist", true);
    }
    bool dummy;
    if(!pblocktree->ReadFlag("couponlist", dummy))
        return false;
    return true;

}

bool InitServiceUbiList(CCoinsView &dbview)
{
    LOCK(cs_main);
    if (fReindex || chainActive.Genesis() == NULL) {
        if(!dbview.SetUbiList(string("ubi"),std::make_tuple("1", "0"))) {
            return false; }
        pblocktree->WriteFlag("ubilist", true);
    }
    bool dummy;
    if(!pblocktree->ReadFlag("ubilist", dummy))
        return false;
    return true;

}

bool InitServiceDexList(CCoinsView &dbview)
{
    LOCK(cs_main);
    if (fReindex || chainActive.Genesis() == NULL) {
        if(!dbview.SetDexList(string("dex"),std::make_tuple("1", "0", "0"))) {
            return false; }
        pblocktree->WriteFlag("dexlist", true);
    }
    bool dummy;
    if(!pblocktree->ReadFlag("dexlist", dummy))
        return false;
    return true;

}

bool InitServiceBookList(CCoinsView &dbview)
{
    LOCK(cs_main);
    if (fReindex || chainActive.Genesis() == NULL) {
        if(!dbview.SetBookList(string("chapter"), std::make_tuple("1", "0", "0"))) {
            return false; }
        pblocktree->WriteFlag("booklist", true);
    }
    bool dummy;
    if(!pblocktree->ReadFlag("booklist", dummy))
        return false;
    return true;

}

bool InitServiceNPList(CCoinsView &dbview)
{
    LOCK(cs_main);
    if (fReindex || chainActive.Genesis() == NULL) {
        if(!dbview.SetBookList(string("np"), std::make_tuple("1", "0", "0"))) {
            return false; }
        pblocktree->WriteFlag("nplist", true);
    }
    bool dummy;
    if(!pblocktree->ReadFlag("nplist", dummy))
        return false;
    return true;

}

void PrintBlockTree()
{
	AssertLockHeld(cs_main);
	// pre-compute tree structure
	map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
	for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
	{
		CBlockIndex* pindex = (*mi).second;
		mapNext[pindex->pprev].push_back(pindex);
	}

	vector<pair<int, CBlockIndex*> > vStack;
	vStack.push_back(make_pair(0, chainActive.Genesis()));

	int nPrevCol = 0;
	while (!vStack.empty())
	{
		int nCol = vStack.back().first;
		CBlockIndex* pindex = vStack.back().second;
		vStack.pop_back();

		// print split or gap
		if (nCol > nPrevCol)
		{
			for (int i = 0; i < nCol-1; i++)
				LogPrintf("| ");
			LogPrintf("|\\\n");
		}
		else if (nCol < nPrevCol)
		{
			for (int i = 0; i < nCol; i++)
				LogPrintf("| ");
			LogPrintf("|\n");
		}
		nPrevCol = nCol;

		// print columns
		for (int i = 0; i < nCol; i++)
			LogPrintf("| ");

		// print item
		CBlock block;
		ReadBlockFromDisk(block, pindex);
		LogPrintf("%d (blk%05u.dat:0x%x)  %s  tx %u\n",
				pindex->nHeight,
				pindex->GetBlockPos().nFile, pindex->GetBlockPos().nPos,
				DateTimeStrFormat("%Y-%m-%d %H:%M:%S", block.GetBlockTime()),
				block.vtx.size());

		// put the main time-chain first
		vector<CBlockIndex*>& vNext = mapNext[pindex];
		for (unsigned int i = 0; i < vNext.size(); i++)
		{
			if (chainActive.Next(vNext[i]))
			{
				swap(vNext[0], vNext[i]);
				break;
			}
		}

		// iterate children
		for (unsigned int i = 0; i < vNext.size(); i++)
			vStack.push_back(make_pair(nCol+i, vNext[i]));
	}
}

bool LoadExternalBlockFile(FILE* fileIn, CDiskBlockPos *dbp)
{
	int64_t nStart = GetTimeMillis();

	int nLoaded = 0;
	try {

		/*const unsigned int maxBlockSize=MAX_BLOCK_SIZE_8;*/

		CBufferedFile blkdat(fileIn, 2*MAX_BLOCK_SIZE, MAX_BLOCK_SIZE+8, SER_DISK, CLIENT_VERSION);
		uint64_t nStartByte = 0;
		if (dbp) {
			// (try to) skip already indexed part
			CBlockFileInfo info;
			if (pblocktree->ReadBlockFileInfo(dbp->nFile, info)) {
				nStartByte = info.nSize;
				blkdat.Seek(info.nSize);
			}
		}
		uint64_t nRewind = blkdat.GetPos();
		while (blkdat.good() && !blkdat.eof()) {
			boost::this_thread::interruption_point();

			blkdat.SetPos(nRewind);
			nRewind++; // start one byte further next time, in case of failure
			blkdat.SetLimit(); // remove former limit
			unsigned int nSize = 0;
			try {
				// locate a header
				unsigned char buf[MESSAGE_START_SIZE];
				blkdat.FindByte(Params().MessageStart()[0]);
				nRewind = blkdat.GetPos()+1;
				blkdat >> FLATDATA(buf);
				if (memcmp(buf, Params().MessageStart(), MESSAGE_START_SIZE))
					continue;
				// read size
				blkdat >> nSize;
				if (nSize < 80 || nSize > MAX_BLOCK_SIZE)
					continue;
			} catch (std::exception &e) {
				// no valid block header found; don't complain
				break;
			}
			try {
				// read block
				uint64_t nBlockPos = blkdat.GetPos();
				blkdat.SetLimit(nBlockPos + nSize);
				CBlock block;
				blkdat >> block;
				nRewind = blkdat.GetPos();

				// process block
				if (nBlockPos >= nStartByte) {
					LOCK(cs_main);
					if (dbp)
						dbp->nPos = nBlockPos;
					CValidationState state;
					if (ProcessBlock(state, NULL, &block, dbp))
						nLoaded++;
					if (state.IsError())
						break;
				}
			} catch (std::exception &e) {
				LogPrintf("%s : Deserialize or I/O error - %s", __func__, e.what());
			}
		}
		fclose(fileIn);
	} catch(std::runtime_error &e) {
		AbortNode(_("Error: system error: ") + e.what());
	}
	if (nLoaded > 0)
		LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
	return nLoaded > 0;
}

//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

string GetWarnings(string strFor)
{
	int nPriority = 0;
	string strStatusBar;
	string strRPC;

	if (GetBoolArg("-testsafemode", false))
		strRPC = "test";

	if (!CLIENT_VERSION_IS_RELEASE)
		strStatusBar = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");

	// Misc warnings like out of disk space and clock is wrong
	if (strMiscWarning != "")
	{
		nPriority = 1000;
		strStatusBar = strMiscWarning;
	}

	if (fLargeWorkForkFound)
	{
		nPriority = 2000;
		strStatusBar = strRPC = _("Warning: The network does not appear to fully agree! Some miners appear to be experiencing issues.");
	}
	else if (fLargeWorkInvalidChainFound)
	{
		nPriority = 2000;
		strStatusBar = strRPC = _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.");
	}

	// Alerts
	{
		LOCK(cs_mapAlerts);
		BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
		{
			const CAlert& alert = item.second;
			if (alert.AppliesToMe() && alert.nPriority > nPriority)
			{
				nPriority = alert.nPriority;
				strStatusBar = alert.strStatusBar;
			}
		}
	}

	if (strFor == "statusbar")
		return strStatusBar;
	else if (strFor == "rpc")
		return strRPC;
	assert(!"GetWarnings() : invalid parameter");
	return "error";
}

//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(const CInv& inv)
						{
	switch (inv.type)
	{
	case MSG_TX:
	{
		bool txInMap = false;
		txInMap = mempool.exists(inv.hash);
		return txInMap || mapOrphanTransactions.count(inv.hash) ||
				pcoinsTip->HaveCoins(inv.hash);
	}
	case MSG_BLOCK:
		return mapBlockIndex.count(inv.hash) ||
				mapOrphanBlocks.count(inv.hash);
	}
	// Don't know what it is, just say we already got one
	return true;
						}
void static ProcessGetData(CNode* pfrom)
						{
	std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

	vector<CInv> vNotFound;

	LOCK(cs_main);

	while (it != pfrom->vRecvGetData.end()) {
		// Don't bother if send buffer is too full to respond anyway
		if (pfrom->nSendSize >= SendBufferSize())
			break;

		const CInv &inv = *it;
		{
			boost::this_thread::interruption_point();
			it++;

			if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
			{
				bool send = false;
				map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(inv.hash);
				if (mi != mapBlockIndex.end())
				{
					// If the requested block is at a height below our last
					// checkpoint, only serve it if it's in the checkpointed chain
					int nHeight = mi->second->nHeight;
					CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
					if (pcheckpoint && nHeight < pcheckpoint->nHeight) {
						if (!chainActive.Contains(mi->second))
						{
							LogPrintf("ProcessGetData(): ignoring request for old block that isn't in the main chain\n");
						} else {
							send = true;
						}
					} else {
						send = true;
					}
				}
				if (send)
				{
					// Send block from disk
					CBlock block;
					ReadBlockFromDisk(block, (*mi).second);

					if (inv.type == MSG_BLOCK)
						pfrom->PushMessage("block", block);
					else // MSG_FILTERED_BLOCK)
					{
						LOCK(pfrom->cs_filter);
						if (pfrom->pfilter)
						{
							CMerkleBlock merkleBlock(block, *pfrom->pfilter);
							pfrom->PushMessage("merkleblock", merkleBlock);
							typedef std::pair<unsigned int, uint256> PairType;
							BOOST_FOREACH(PairType& pair, merkleBlock.vMatchedTxn)
							if (!pfrom->setInventoryKnown.count(CInv(MSG_TX, pair.second)))
								pfrom->PushMessage("tx", block.vtx[pair.first]);
						}
					}

					// Trigger them to send a getblocks request for the next batch of inventory
					if (inv.hash == pfrom->hashContinue)
					{
						// Bypass PushInventory.
						vector<CInv> vInv;
						vInv.push_back(CInv(MSG_BLOCK, chainActive.Tip()->GetBlockHash()));
						pfrom->PushMessage("inv", vInv);
						pfrom->hashContinue = 0;
					}
				}
			}
			else if (inv.IsKnownType())
			{
				// Send stream from relay memory
				bool pushed = false;
				{
					LOCK(cs_mapRelay);
					map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
					if (mi != mapRelay.end()) {
						pfrom->PushMessage(inv.GetCommand(), (*mi).second);
						pushed = true;
					}
				}
				if (!pushed && inv.type == MSG_TX) {
					CTransaction tx;
					if (mempool.lookup(inv.hash, tx)) {
						CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
						ss.reserve(1000);
						ss << tx;
						pfrom->PushMessage("tx", ss);
						pushed = true;
					}
				}
				if (!pushed) {
					vNotFound.push_back(inv);
				}
			}

			// Track requests for our stuff.
			g_signals.Inventory(inv.hash);

			if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
				break;
		}
	}

	pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

	if (!vNotFound.empty()) {
		// Let the peer know that we didn't find what it asked for, so it doesn't have to wait around forever.
		pfrom->PushMessage("notfound", vNotFound);
	}
						}

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv)
						{
	RandAddSeedPerfmon();
	LogPrint("net", "received: %s (%u bytes)\n", strCommand, vRecv.size());
	if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
	{
		LogPrintf("dropmessagestest DROPPING RECV MESSAGE\n");
		return true;
	}

	{
		LOCK(cs_main);
		State(pfrom->GetId())->nLastBlockProcess = GetTimeMicros();
	}

	if (strCommand == "version")
	{
		// Each connection can only send one version message
		if (pfrom->nVersion != 0)
		{
			pfrom->PushMessage("reject", strCommand, REJECT_DUPLICATE, string("Duplicate version message"));
			Misbehaving(pfrom->GetId(), 1);
			return false;
		}

		int64_t nTime;
		CAddress addrMe;
		CAddress addrFrom;
		uint64_t nNonce = 1;
		vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;

		if (GetHeight() < nRichForkHeight) //TODO: ?
		{
		 if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
		 {
		   // disconnect from peers older than this proto version
			 LogPrintf("partner %s using obsolete version %i; disconnecting\n", pfrom->addr.ToString(), pfrom->nVersion);
			 pfrom->PushMessage("reject", strCommand, REJECT_OBSOLETE,
				  strprintf("Version must be %d or greater", MIN_PEER_PROTO_VERSION));
			 pfrom->fDisconnect = true;
			 return false;
		 }
	 }else{
		 if (pfrom->nVersion < MIN_PEER_PROTO_VERSION_POST_CHANGE)
		 {
		   // disconnect from peers older than this proto version
			 LogPrintf("partner %s using obsolete version %i; disconnecting\n", pfrom->addr.ToString(), pfrom->nVersion);
			 pfrom->PushMessage("reject", strCommand, REJECT_OBSOLETE,
				  strprintf("Version must be %d or greater", MIN_PEER_PROTO_VERSION_POST_CHANGE));
			 pfrom->fDisconnect = true;
			 return false;
		 }
	 }

		if (pfrom->nVersion == 10300)
			pfrom->nVersion = 300;
		if (!vRecv.empty())
			vRecv >> addrFrom >> nNonce;
		if (!vRecv.empty()) {
			vRecv >> pfrom->strSubVer;
			pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);

			if ( // TODO: throw out or put in relevant versions?
					(pfrom->cleanSubVer == "/Aurora:1.3.0/") ||
					(pfrom->cleanSubVer == "/Arngrímur Jónsson:0.8.7.5/") ||
					(pfrom->cleanSubVer == "/Satoshi:0.8.7.5/")
			)
			{
				// disconnect from peers older than this proto version
				LogPrintf("partner %s using obsolete sub version %s; disconnecting\n", pfrom->addr.ToString(), pfrom->cleanSubVer);
				pfrom->PushMessage("reject", strCommand, REJECT_OBSOLETE,
						strprintf("Version must be %d or greater", MIN_PEER_PROTO_VERSION));
				pfrom->fDisconnect = true;
				return false;
			}
		}
		if (!vRecv.empty())
			vRecv >> pfrom->nStartingHeight;
		if (!vRecv.empty())
			vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
		else
			pfrom->fRelayTxes = true;

		if (pfrom->fInbound && addrMe.IsRoutable())
		{
			pfrom->addrLocal = addrMe;
			SeenLocal(addrMe);
		}

		// Disconnect if we connected to ourself
		if (nNonce == nLocalHostNonce && nNonce > 1)
		{
			LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
			pfrom->fDisconnect = true;
			return true;
		}

		// Be shy and don't send version until we hear
		if (pfrom->fInbound)
			pfrom->PushVersion();

		pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);


		// Change version
		pfrom->PushMessage("verack");
		pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

		if (!pfrom->fInbound)
		{
			// Advertise our address
			if (!fNoListen && !IsInitialBlockDownload())
			{
				CAddress addr = GetLocalAddress(&pfrom->addr);
				if (addr.IsRoutable())
					pfrom->PushAddress(addr);
			}

			// Get recent addresses
			if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
			{
				pfrom->PushMessage("getaddr");
				pfrom->fGetAddr = true;
			}
			addrman.Good(pfrom->addr);
		} else {
			if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
			{
				addrman.Add(addrFrom, addrFrom);
				addrman.Good(addrFrom);
			}
		}

		// Relay alerts
		{
			LOCK(cs_mapAlerts);
			BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
			item.second.RelayTo(pfrom);
		}

		pfrom->fSuccessfullyConnected = true;

		LogPrintf("receive version message: %s: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", pfrom->cleanSubVer, pfrom->nVersion, pfrom->nStartingHeight, addrMe.ToString(), addrFrom.ToString(), pfrom->addr.ToString());

		AddTimeData(pfrom->addr, nTime);

		LOCK(cs_main);
		cPeerBlockCounts.input(pfrom->nStartingHeight);
	}


	else if (pfrom->nVersion == 0)
	{
		// Must have a version message before anything else
		Misbehaving(pfrom->GetId(), 1);
		return false;
	}

	else if (strCommand == "verack")
	{
		pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));
	}

	else if (strCommand == "addr")
	{
		vector<CAddress> vAddr;
		vRecv >> vAddr;

		// Don't want addr from older versions unless seeding
		if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
			return true;
		if (vAddr.size() > 1000)
		{
			Misbehaving(pfrom->GetId(), 20);
			return error("message addr size() = %u", vAddr.size());
		}

		// Store the new addresses
		vector<CAddress> vAddrOk;
		int64_t nNow = GetAdjustedTime();
		int64_t nSince = nNow - 10 * 60;
		BOOST_FOREACH(CAddress& addr, vAddr)
		{
			boost::this_thread::interruption_point();

			if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
				addr.nTime = nNow - 5 * 24 * 60 * 60;
			pfrom->AddAddressKnown(addr);
			bool fReachable = IsReachable(addr);
			if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
			{
				// Relay to a limited number of other nodes
				{
					LOCK(cs_vNodes);
					// Use deterministic randomness to send to the same nodes for 24 hours
					// at a time so the setAddrKnowns of the chosen nodes prevent repeats
					static uint256 hashSalt;
					if (hashSalt == 0)
						hashSalt = GetRandHash();
					uint64_t hashAddr = addr.GetHash();
					uint256 hashRand = hashSalt ^ (hashAddr<<32) ^ ((GetTime()+hashAddr)/(24*60*60));
					hashRand = Hash(BEGIN(hashRand), END(hashRand));
					multimap<uint256, CNode*> mapMix;
					BOOST_FOREACH(CNode* pnode, vNodes)
					{
						if (pnode->nVersion < CADDR_TIME_VERSION)
							continue;
						unsigned int nPointer;
						memcpy(&nPointer, &pnode, sizeof(nPointer));
						uint256 hashKey = hashRand ^ nPointer;
						hashKey = Hash(BEGIN(hashKey), END(hashKey));
						mapMix.insert(make_pair(hashKey, pnode));
					}
					int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
					for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
						((*mi).second)->PushAddress(addr);
				}
			}
			// Do not store addresses outside our network
			if (fReachable)
				vAddrOk.push_back(addr);
		}
		addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
		if (vAddr.size() < 1000)
			pfrom->fGetAddr = false;
		if (pfrom->fOneShot)
			pfrom->fDisconnect = true;
	}
	else if (strCommand == "inv")
	{
		vector<CInv> vInv;
		vRecv >> vInv;
		if (vInv.size() > MAX_INV_SZ)
		{
			Misbehaving(pfrom->GetId(), 20);
			return error("message inv size() = %u", vInv.size());
		}

		LOCK(cs_main);

		for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
		{
			const CInv &inv = vInv[nInv];

			boost::this_thread::interruption_point();
			pfrom->AddInventoryKnown(inv);

			bool fAlreadyHave = AlreadyHave(inv);
			LogPrint("net", "  got inventory: %s  %s\n", inv.ToString(), fAlreadyHave ? "have" : "new");

			if (!fAlreadyHave) {
				if (!fImporting && !fReindex) {
					if (inv.type == MSG_BLOCK)
					{
						AddBlockToQueue(pfrom->GetId(), inv.hash);
					}
					else
						pfrom->AskFor(inv);
				}
			} else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash)) {
				PushGetBlocks(pfrom, chainActive.Tip(), GetOrphanRoot(inv.hash));
			}

			// Track requests for our stuff
			g_signals.Inventory(inv.hash);
		}
	}
	else if (strCommand == "getdata")
	{
		vector<CInv> vInv;
		vRecv >> vInv;
		if (vInv.size() > MAX_INV_SZ)
		{
			Misbehaving(pfrom->GetId(), 20);
			return error("message getdata size() = %u", vInv.size());
		}

		if (fDebug || (vInv.size() != 1))
			LogPrint("net", "received getdata (%u invsz)\n", vInv.size());

		if ((fDebug && vInv.size() > 0) || (vInv.size() == 1))
			LogPrint("net", "received getdata for: %s\n", vInv[0].ToString());

		pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
		ProcessGetData(pfrom);
	}
	else if (strCommand == "getblocks")
	{
		CBlockLocator locator;
		uint256 hashStop;
		vRecv >> locator >> hashStop;

		LOCK(cs_main);

		// Find the last block the caller has in the main chain
		CBlockIndex* pindex = chainActive.FindFork(locator);

		// Send the rest of the chain
		if (pindex)
			pindex = chainActive.Next(pindex);
		int nLimit = 500;
		LogPrint("net", "getblocks %d to %s limit %d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), nLimit);
		for (; pindex; pindex = chainActive.Next(pindex))
		{
			if (pindex->GetBlockHash() == hashStop)
			{
				LogPrint("net", "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
				break;
			}
			pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
			if (--nLimit <= 0)
			{
				// When this block is requested, we'll send an inv that'll make them
				// getblocks the next batch of inventory.
				LogPrint("net", "  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
				pfrom->hashContinue = pindex->GetBlockHash();
				break;
			}
		}
	}
	else if (strCommand == "getheaders")
	{
		CBlockLocator locator;
		uint256 hashStop;
		vRecv >> locator >> hashStop;

		LOCK(cs_main);

		CBlockIndex* pindex = NULL;
		if (locator.IsNull())
		{
			// If locator is null, return the hashStop block
			map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashStop);
			if (mi == mapBlockIndex.end())
				return true;
			pindex = (*mi).second;
		}
		else
		{
			// Find the last block the caller has in the main chain
			pindex = chainActive.FindFork(locator);
			if (pindex)
				pindex = chainActive.Next(pindex);
		}

		// we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
		vector<CBlock> vHeaders;
		int nLimit = 2000;
		LogPrint("net", "getheaders %d to %s\n", (pindex ? pindex->nHeight : -1), hashStop.ToString());
		for (; pindex; pindex = chainActive.Next(pindex))
		{
			vHeaders.push_back(pindex->GetBlockHeader());
			if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
				break;
		}
		pfrom->PushMessage("headers", vHeaders);
	}
	else if (strCommand == "tx")
	{
		vector<uint256> vWorkQueue;
		vector<uint256> vEraseQueue;
		CTransaction tx;
		vRecv >> tx;

		CInv inv(MSG_TX, tx.GetHash());
		pfrom->AddInventoryKnown(inv);

		LOCK(cs_main);

		bool fMissingInputs = false;
		CValidationState state;
		if (AcceptToMemoryPool(mempool, state, tx, true, &fMissingInputs))
		{
			mempool.check(pcoinsTip);
			RelayTransaction(tx, inv.hash);
			mapAlreadyAskedFor.erase(inv);
			vWorkQueue.push_back(inv.hash);
			vEraseQueue.push_back(inv.hash);

			LogPrint("mempool", "AcceptToMemoryPool: %s %s : accepted %s (poolsz %u)\n",
					pfrom->addr.ToString(), pfrom->cleanSubVer,
					tx.GetHash().ToString(),
					mempool.mapTx.size());

			// Recursively process any orphan transactions that depended on this one
			for (unsigned int i = 0; i < vWorkQueue.size(); i++)
			{
				uint256 hashPrev = vWorkQueue[i];
				for (set<uint256>::iterator mi = mapOrphanTransactionsByPrev[hashPrev].begin();
						mi != mapOrphanTransactionsByPrev[hashPrev].end();
						++mi)
				{
					const uint256& orphanHash = *mi;
					const CTransaction& orphanTx = mapOrphanTransactions[orphanHash];
					bool fMissingInputs2 = false;
					// Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan resolution
					CValidationState stateDummy;

					if (AcceptToMemoryPool(mempool, stateDummy, orphanTx, true, &fMissingInputs2))
					{
						LogPrint("mempool", "   accepted orphan tx %s\n", orphanHash.ToString());
						RelayTransaction(orphanTx, orphanHash);
						mapAlreadyAskedFor.erase(CInv(MSG_TX, orphanHash));
						vWorkQueue.push_back(orphanHash);
						vEraseQueue.push_back(orphanHash);
					}
					else if (!fMissingInputs2)
					{
						// invalid or too-little-fee orphan
						vEraseQueue.push_back(orphanHash);
						LogPrint("mempool", "   removed orphan tx %s\n", orphanHash.ToString());
					}
					mempool.check(pcoinsTip);
				}
			}

			BOOST_FOREACH(uint256 hash, vEraseQueue)
			EraseOrphanTx(hash);
		}
		else if (fMissingInputs)
		{
			AddOrphanTx(tx);

			int maxBlockSize;

			maxBlockSize=GetMaxBlockSize(chainActive.Height());

			// DoS prevention: do not allow mapOrphanTransactions to grow unbounded
			unsigned int nEvicted = LimitOrphanTxSize(maxBlockSize/100);
			if (nEvicted > 0)
				LogPrint("mempool", "mapOrphan overflow, removed %u tx\n", nEvicted);
		}

		int nDoS = 0;
		if (state.IsInvalid(nDoS))
		{
			LogPrint("mempool", "%s from %s %s was not accepted into the memory pool: %s\n", tx.GetHash().ToString(),
					pfrom->addr.ToString(), pfrom->cleanSubVer,
					state.GetRejectReason());
			pfrom->PushMessage("reject", strCommand, state.GetRejectCode(),
					state.GetRejectReason(), inv.hash);
			if (nDoS > 0)
				Misbehaving(pfrom->GetId(), nDoS);
		}
	}
	else if (strCommand == "block" && !fImporting && !fReindex) // Ignore blocks received while importing
	{
		CBlock block;
		vRecv >> block;

		LogPrint("net", "received block %s\n", block.GetHash().ToString());

		CInv inv(MSG_BLOCK, block.GetHash());
		pfrom->AddInventoryKnown(inv);

		LOCK(cs_main);
		// Remember who we got this block from.
		mapBlockSource[inv.hash] = pfrom->GetId();
		MarkBlockAsReceived(inv.hash, pfrom->GetId());

		CValidationState state;
		ProcessBlock(state, pfrom, &block);
	}
	else if (strCommand == "getaddr")
	{
		pfrom->vAddrToSend.clear();
		vector<CAddress> vAddr = addrman.GetAddr();
		BOOST_FOREACH(const CAddress &addr, vAddr)
		pfrom->PushAddress(addr);
	}
	else if (strCommand == "mempool")
	{
		LOCK2(cs_main, pfrom->cs_filter);

		std::vector<uint256> vtxid;
		mempool.queryHashes(vtxid);
		vector<CInv> vInv;
		BOOST_FOREACH(uint256& hash, vtxid) {
			CInv inv(MSG_TX, hash);
			CTransaction tx;
			bool fInMemPool = mempool.lookup(hash, tx);
			if (!fInMemPool) continue; // another thread removed since queryHashes, maybe...
			if ((pfrom->pfilter && pfrom->pfilter->IsRelevantAndUpdate(tx, hash)) ||
					(!pfrom->pfilter))
				vInv.push_back(inv);
			if (vInv.size() == MAX_INV_SZ) {
				pfrom->PushMessage("inv", vInv);
				vInv.clear();
			}
		}
		if (vInv.size() > 0)
			pfrom->PushMessage("inv", vInv);
	}
	else if (strCommand == "ping")
	{
		if (pfrom->nVersion > BIP0031_VERSION)
		{
			uint64_t nonce = 0;
			vRecv >> nonce;
			// Echo the message back with the nonce.
			pfrom->PushMessage("pong", nonce);
		}
	}
	else if (strCommand == "pong")
	{
		int64_t pingUsecEnd = GetTimeMicros();
		uint64_t nonce = 0;
		size_t nAvail = vRecv.in_avail();
		bool bPingFinished = false;
		std::string sProblem;

		if (nAvail >= sizeof(nonce)) {
			vRecv >> nonce;

			// Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
			if (pfrom->nPingNonceSent != 0) {
				if (nonce == pfrom->nPingNonceSent) {
					// Matching pong received, this ping is no longer outstanding
					bPingFinished = true;
					int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
					if (pingUsecTime > 0) {
						// Successful ping time measurement, replace previous
						pfrom->nPingUsecTime = pingUsecTime;
					} else {
						// This should never happen
						sProblem = "Timing mishap";
					}
				} else {
					// Nonce mismatches are normal when pings are overlapping
					sProblem = "Nonce mismatch";
					if (nonce == 0) {
						// This is most likely a bug in another implementation somewhere, cancel this ping
						bPingFinished = true;
						sProblem = "Nonce zero";
					}
				}
			} else {
				sProblem = "Unsolicited pong without ping";
			}
		} else {
			// This is most likely a bug in another implementation somewhere, cancel this ping
			bPingFinished = true;
			sProblem = "Short payload";
		}

		if (!(sProblem.empty())) {
			LogPrint("net", "pong %s %s: %s, %x expected, %x received, %u bytes\n",
					pfrom->addr.ToString(),
					pfrom->cleanSubVer,
					sProblem,
					pfrom->nPingNonceSent,
					nonce,
					nAvail);
		}
		if (bPingFinished) {
			pfrom->nPingNonceSent = 0;
		}
	}
	else if (strCommand == "alert")
	{
		CAlert alert;
		vRecv >> alert;

		uint256 alertHash = alert.GetHash();
		if (pfrom->setKnown.count(alertHash) == 0)
		{
			if (alert.ProcessAlert())
			{
				// Relay
				pfrom->setKnown.insert(alertHash);
				{
					LOCK(cs_vNodes);
					BOOST_FOREACH(CNode* pnode, vNodes)
					alert.RelayTo(pnode);
				}
			}
			else {
				// Small DoS penalty so peers that send numerous duplicate/expired/invalid-signature alerts eventually get banned.
				Misbehaving(pfrom->GetId(), 10);
			}
		}
	}


	else if (strCommand == "filterload")
	{
		CBloomFilter filter;
		vRecv >> filter;

		if (!filter.IsWithinSizeConstraints())
			// There is no excuse for sending a too-large filter
			Misbehaving(pfrom->GetId(), 100);
		else
		{
			LOCK(pfrom->cs_filter);
			delete pfrom->pfilter;
			pfrom->pfilter = new CBloomFilter(filter);
			pfrom->pfilter->UpdateEmptyFull();
		}
		pfrom->fRelayTxes = true;
	}


	else if (strCommand == "filteradd")
	{
		vector<unsigned char> vData;
		vRecv >> vData;

		// Nodes must NEVER send a data item > 520 bytes in a filteradd message
		if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE)
		{
			Misbehaving(pfrom->GetId(), 100);
		} else {
			LOCK(pfrom->cs_filter);
			if (pfrom->pfilter)
				pfrom->pfilter->insert(vData);
			else
				Misbehaving(pfrom->GetId(), 100);
		}
	}

	else if (strCommand == "filterclear")
	{
		LOCK(pfrom->cs_filter);
		delete pfrom->pfilter;
		pfrom->pfilter = new CBloomFilter();
		pfrom->fRelayTxes = true;
	}

	else if (strCommand == "reject")
	{
		if (fDebug)
		{
			string strMsg; unsigned char ccode; string strReason;
			vRecv >> strMsg >> ccode >> strReason;

			ostringstream ss;
			ss << strMsg << " code " << itostr(ccode) << ": " << strReason;

			if (strMsg == "block" || strMsg == "tx")
			{
				uint256 hash;
				vRecv >> hash;
				ss << ": hash " << hash.ToString();
			}
			// Truncate to reasonable length and sanitize before printing:
			string s = ss.str();
			if (s.size() > 111) s.erase(111, string::npos);
			LogPrint("net", "Reject %s\n", SanitizeString(s));
		}
	}

	else
	{
		// Ignore unknown commands for extensibility
	}

	// Update the last seen time for this node's address
	if (pfrom->fNetworkNode)
		if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
			AddressCurrentlyConnected(pfrom->addr);

	return true;
						}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CNode* pfrom)
{
	bool fOk = true;

	if (!pfrom->vRecvGetData.empty())
		ProcessGetData(pfrom);

	// this maintains the order of responses
	if (!pfrom->vRecvGetData.empty()) return fOk;

	std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
	while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end()) {
		// Don't bother if send buffer is too full to respond anyway
		if (pfrom->nSendSize >= SendBufferSize())
			break;

		// get next message
		CNetMessage& msg = *it;

		// end, if an incomplete message is found
		if (!msg.complete())
			break;

		// at this point, any failure means we can delete the current message
		it++;

		// Scan for message start
		if (memcmp(msg.hdr.pchMessageStart, Params().MessageStart(), MESSAGE_START_SIZE) != 0) {
			LogPrintf("\n\nPROCESSMESSAGE: INVALID MESSAGESTART\n\n");
			fOk = false;
			break;
		}

		// Read header
		CMessageHeader& hdr = msg.hdr;
		if (!hdr.IsValid())
		{
			LogPrintf("\n\nPROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", hdr.GetCommand());
			continue;
		}
		string strCommand = hdr.GetCommand();

		// Message size
		unsigned int nMessageSize = hdr.nMessageSize;

		// Checksum
		CDataStream& vRecv = msg.vRecv;
		uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
		unsigned int nChecksum = 0;
		memcpy(&nChecksum, &hash, sizeof(nChecksum));
		if (nChecksum != hdr.nChecksum)
		{
			LogPrintf("ProcessMessages(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
					strCommand, nMessageSize, nChecksum, hdr.nChecksum);
			continue;
		}

		// Process message
		bool fRet = false;
		try
		{
			fRet = ProcessMessage(pfrom, strCommand, vRecv);
			boost::this_thread::interruption_point();
		}
		catch (std::ios_base::failure& e)
		{
			pfrom->PushMessage("reject", strCommand, REJECT_MALFORMED, string("error parsing message"));
			if (strstr(e.what(), "end of data"))
			{
				// Allow exceptions from under-length message on vRecv
				LogPrintf("ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand, nMessageSize, e.what());
			}
			else if (strstr(e.what(), "size too large"))
			{
				// Allow exceptions from over-long size
				LogPrintf("ProcessMessages(%s, %u bytes) : Exception '%s' caught\n", strCommand, nMessageSize, e.what());
			}
			else
			{
				PrintExceptionContinue(&e, "ProcessMessages()");
			}
		}
		catch (boost::thread_interrupted) {
			throw;
		}
		catch (std::exception& e) {
			PrintExceptionContinue(&e, "ProcessMessages()");
		} catch (...) {
			PrintExceptionContinue(NULL, "ProcessMessages()");
		}

		if (!fRet)
			LogPrintf("ProcessMessage(%s, %u bytes) FAILED\n", strCommand, nMessageSize);

		break;
	}

	// In case the connection got shut down, its receive buffer was wiped
	if (!pfrom->fDisconnect)
		pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

	return fOk;
}

bool SendMessages(CNode* pto, bool fSendTrickle)
{
	{
		// Don't send anything until we get their version message
		if (pto->nVersion == 0)
			return true;

		// Message: ping
		bool pingSend = false;
		if (pto->fPingQueued) {
			// RPC ping request by user
			pingSend = true;
		}
		if (pto->nLastSend && GetTime() - pto->nLastSend > 30 * 60 && pto->vSendMsg.empty()) {
			// Ping automatically sent as a keepalive
			pingSend = true;
		}
		if (pingSend) {
			uint64_t nonce = 0;
			while (nonce == 0) {
				RAND_bytes((unsigned char*)&nonce, sizeof(nonce));
			}
			pto->nPingNonceSent = nonce;
			pto->fPingQueued = false;
			if (pto->nVersion > BIP0031_VERSION) {
				// Take timestamp as close as possible before transmitting ping
				pto->nPingUsecStart = GetTimeMicros();
				pto->PushMessage("ping", nonce);
			} else {
				// Peer is too old to support ping command with nonce, pong will never arrive, disable timing
				pto->nPingUsecStart = 0;
				pto->PushMessage("ping");
			}
		}

		TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
		if (!lockMain)
			return true;

		// Address refresh broadcast
		static int64_t nLastRebroadcast;
		if (!IsInitialBlockDownload() && (GetTime() - nLastRebroadcast > 24 * 60 * 60))
		{
			{
				LOCK(cs_vNodes);
				BOOST_FOREACH(CNode* pnode, vNodes)
				{
					// Periodically clear setAddrKnown to allow refresh broadcasts
					if (nLastRebroadcast)
						pnode->setAddrKnown.clear();

					// Rebroadcast our address
					if (!fNoListen)
					{
						CAddress addr = GetLocalAddress(&pnode->addr);
						if (addr.IsRoutable())
							pnode->PushAddress(addr);
					}
				}
			}
			nLastRebroadcast = GetTime();
		}

		// Message: addr
		if (fSendTrickle)
		{
			vector<CAddress> vAddr;
			vAddr.reserve(pto->vAddrToSend.size());
			BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
			{
				// returns true if wasn't already contained in the set
				if (pto->setAddrKnown.insert(addr).second)
				{
					vAddr.push_back(addr);
					// receiver rejects addr messages larger than 1000
					if (vAddr.size() >= 1000)
					{
						pto->PushMessage("addr", vAddr);
						vAddr.clear();
					}
				}
			}
			pto->vAddrToSend.clear();
			if (!vAddr.empty())
				pto->PushMessage("addr", vAddr);
		}

		CNodeState &state = *State(pto->GetId());
		if (state.fShouldBan) {
			if (pto->addr.IsLocal())
				LogPrintf("Warning: not banning local node %s!\n", pto->addr.ToString());
			else {
				pto->fDisconnect = true;
				CNode::Ban(pto->addr);
			}
			state.fShouldBan = false;
		}

		BOOST_FOREACH(const CBlockReject& reject, state.rejects)
		pto->PushMessage("reject", (string)"block", reject.chRejectCode, reject.strRejectReason, reject.hashBlock);
		state.rejects.clear();

		// Start block sync
		if (pto->fStartSync && !fImporting && !fReindex) {
			pto->fStartSync = false;
			PushGetBlocks(pto, chainActive.Tip(), uint256(0));
		}

		// Resend wallet transactions that haven't gotten in a block yet
		if (!fReindex && !fImporting && !IsInitialBlockDownload())
		{
			g_signals.Broadcast();
		}

		// Message: inventory
		vector<CInv> vInv;
		vector<CInv> vInvWait;
		{
			LOCK(pto->cs_inventory);
			vInv.reserve(pto->vInventoryToSend.size());
			vInvWait.reserve(pto->vInventoryToSend.size());
			BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend)
			{
				if (pto->setInventoryKnown.count(inv))
					continue;

				// trickle out tx inv to protect privacy
				if (inv.type == MSG_TX && !fSendTrickle)
				{
					// 1/4 of tx invs blast to all immediately
					static uint256 hashSalt;
					if (hashSalt == 0)
						hashSalt = GetRandHash();
					uint256 hashRand = inv.hash ^ hashSalt;
					hashRand = Hash(BEGIN(hashRand), END(hashRand));
					bool fTrickleWait = ((hashRand & 3) != 0);

					if (fTrickleWait)
					{
						vInvWait.push_back(inv);
						continue;
					}
				}

				// returns true if wasn't already contained in the set
				if (pto->setInventoryKnown.insert(inv).second)
				{
					vInv.push_back(inv);
					if (vInv.size() >= 1000)
					{
						pto->PushMessage("inv", vInv);
						vInv.clear();
					}
				}
			}
			pto->vInventoryToSend = vInvWait;
		}
		if (!vInv.empty())
			pto->PushMessage("inv", vInv);


		// Detect stalled peers.
		int64_t nNow = GetTimeMicros();
		if (!pto->fDisconnect && state.nBlocksInFlight &&
				state.nLastBlockReceive < state.nLastBlockProcess - BLOCK_DOWNLOAD_TIMEOUT*1000000 &&
				state.vBlocksInFlight.front().nTime < state.nLastBlockProcess - 2*BLOCK_DOWNLOAD_TIMEOUT*1000000) {
			LogPrintf("Peer %s is stalling block download, disconnecting\n", state.name.c_str());
			pto->fDisconnect = true;
		}

		// Message: getdata (blocks)
		vector<CInv> vGetData;
		while (!pto->fDisconnect && state.nBlocksToDownload && state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
			uint256 hash = state.vBlocksToDownload.front();
			vGetData.push_back(CInv(MSG_BLOCK, hash));
			MarkBlockAsInFlight(pto->GetId(), hash);
			LogPrint("net", "Requesting block %s from %s\n", hash.ToString().c_str(), state.name.c_str());
			if (vGetData.size() >= 1000)
			{
				pto->PushMessage("getdata", vGetData);
				vGetData.clear();
			}
		}

		// Message: getdata (non-blocks)
		while (!pto->fDisconnect && !pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
		{
			const CInv& inv = (*pto->mapAskFor.begin()).second;
			if (!AlreadyHave(inv))
			{
				if (fDebug)
					LogPrint("net", "sending getdata: %s\n", inv.ToString());
				vGetData.push_back(inv);
				if (vGetData.size() >= 1000)
				{
					pto->PushMessage("getdata", vGetData);
					vGetData.clear();
				}
			}
			pto->mapAskFor.erase(pto->mapAskFor.begin());
		}
		if (!vGetData.empty())
			pto->PushMessage("getdata", vGetData);

	}
	return true;
}

class CMainCleanup
{
public:
	CMainCleanup() {}
	~CMainCleanup() {
		// block headers
		std::map<uint256, CBlockIndex*>::iterator it1 = mapBlockIndex.begin();
		for (; it1 != mapBlockIndex.end(); it1++)
			delete (*it1).second;
		mapBlockIndex.clear();

		// orphan blocks
		std::map<uint256, COrphanBlock*>::iterator it2 = mapOrphanBlocks.begin();
		for (; it2 != mapOrphanBlocks.end(); it2++)
			delete (*it2).second;
		mapOrphanBlocks.clear();

		// orphan transactions
		mapOrphanTransactions.clear();
	}
} instance_of_cmaincleanup;
