// Copyright (c) 2019 Duality Blockchain Solutions Developers
// Copyright (c) 2015-2019 The PIVX developers
// Copyright (c) 2012-2013 The PPCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pos/kernel.h"

#include "arith_uint256.h"
#include "chainparams.h"
#include "db.h"
#include "policy/policy.h"
#include "pos/stakeinput.h"
#include "script/interpreter.h"
#include "timedata.h"
#include "uint256.h"
#include "util.h"
#include "utilmoneystr.h"
#include "validation.h"

#include <boost/assign/list_of.hpp>

// v1 modifier interval.
static const int64_t OLD_MODIFIER_INTERVAL = 2087;

// Hard checkpoints of stake modifiers to ensure they are deterministic
static std::map<int, unsigned int> mapStakeModifierCheckpoints =
    boost::assign::map_list_of(0, 0xfd11f4e7u);

// Get the last stake modifier and its generation time from a given block
static bool GetLastStakeModifier(const CBlockIndex* pindex, uint64_t& nStakeModifier, int64_t& nModifierTime)
{
    if (!pindex)
        return error("%s : null pindex", __func__);

    while (pindex && pindex->pprev && !pindex->GeneratedStakeModifier())
        pindex = pindex->pprev;
    if (!pindex->GeneratedStakeModifier())
        return error("%s : no generation at genesis block", __func__);
    nStakeModifier = pindex->nStakeModifier;
    nModifierTime = pindex->GetBlockTime();

    return true;
}

// Get selection interval section (in seconds)
static int64_t GetStakeModifierSelectionIntervalSection(int nSection)
{
    assert(nSection >= 0 && nSection < 64);
    int64_t a = MODIFIER_INTERVAL  * 63 / (63 + ((63 - nSection) * (MODIFIER_INTERVAL_RATIO - 1)));
    return a;
}

// select a block from the candidate blocks in vSortedByTimestamp, excluding
// already selected blocks in vSelectedBlocks, and with timestamp up to
// nSelectionIntervalStop.
static bool SelectBlockFromCandidates(
    std::vector<std::pair<int64_t, uint256> >& vSortedByTimestamp,
    std::map<uint256, const CBlockIndex*>& mapSelectedBlocks,
    int64_t nSelectionIntervalStop,
    uint64_t nStakeModifierPrev,
    const CBlockIndex** pindexSelected)
{
    bool fSelected = false;
    uint256 hashBest = uint256Zero;
    *pindexSelected = (const CBlockIndex*)0;
    for (const PAIRTYPE(int64_t, uint256)& item : vSortedByTimestamp) {
        if (!mapBlockIndex.count(item.second))
            return error("%s : failed to find block index for candidate block %s", __func__, item.second.ToString().c_str());

        const CBlockIndex* pindex = mapBlockIndex[item.second];
        if (fSelected && pindex->GetBlockTime() > nSelectionIntervalStop)
            break;

        if (mapSelectedBlocks.count(pindex->GetBlockHash()) > 0)
            continue;

        // compute the selection hash by hashing an input that is unique to that block
        uint256 hashProof = pindex->GetBlockHash();

        CDataStream ss(SER_GETHASH, 0);
        ss << hashProof << nStakeModifierPrev;
        uint256 hashSelection = Hash(ss.begin(), ss.end());

        // the selection hash is divided by 2**32 so that proof-of-stake block
        // is always favored over proof-of-work block. this is to preserve
        // the energy efficiency property
        if (pindex->IsProofOfStake())
            hashSelection >>= 32;

        if (fSelected && hashSelection < hashBest) {
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*)pindex;
        } else if (!fSelected) {
            fSelected = true;
            hashBest = hashSelection;
            *pindexSelected = (const CBlockIndex*)pindex;
        }
    }
    if (GetBoolArg("-printstakemodifier", false))
        LogPrintf("%s : selection hash=%s\n", __func__, hashBest.ToString().c_str());
    return fSelected;
}

/* NEW MODIFIER */

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256(); // genesis block's modifier is 0

    CHashWriter ss(SER_GETHASH, 0);
    ss << kernel;
    ss << pindexPrev->nStakeModifierV2;

    return ss.GetHash();
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex* pindexPrev, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier)
{
    nStakeModifier = 0;
    fGeneratedStakeModifier = false;
    if (!pindexPrev) {
        fGeneratedStakeModifier = true;
        return true; // genesis block's modifier is 0
    }
    if (pindexPrev->nHeight == 0) {
        //Give a stake modifier to the first block
        fGeneratedStakeModifier = true;
        nStakeModifier = uint64_t("stakemodifier");
        return true;
    }

    // First find current stake modifier and its generation block time
    // if it's not old enough, return the same stake modifier
    int64_t nModifierTime = 0;
    if (!GetLastStakeModifier(pindexPrev, nStakeModifier, nModifierTime))
        return error("%s : unable to get last modifier", __func__);

    if (GetBoolArg("-printstakemodifier", false))
        LogPrintf("%s : prev modifier= %s time=%s\n", __func__, std::to_string(nStakeModifier).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nModifierTime).c_str());

    if (nModifierTime / MODIFIER_INTERVAL >= pindexPrev->GetBlockTime() / MODIFIER_INTERVAL)
        return true;

    // Sort candidate blocks by timestamp
    std::vector<std::pair<int64_t, uint256> > vSortedByTimestamp;
    vSortedByTimestamp.reserve(64 * MODIFIER_INTERVAL  / Params().TargetPosSpacing());
    int64_t nSelectionIntervalStart = (pindexPrev->GetBlockTime() / MODIFIER_INTERVAL ) * MODIFIER_INTERVAL  - OLD_MODIFIER_INTERVAL;
    const CBlockIndex* pindex = pindexPrev;

    while (pindex && pindex->GetBlockTime() >= nSelectionIntervalStart) {
        vSortedByTimestamp.push_back(std::make_pair(pindex->GetBlockTime(), pindex->GetBlockHash()));
        pindex = pindex->pprev;
    }

    int nHeightFirstCandidate = pindex ? (pindex->nHeight + 1) : 0;
    std::reverse(vSortedByTimestamp.begin(), vSortedByTimestamp.end());
    std::sort(vSortedByTimestamp.begin(), vSortedByTimestamp.end());

    // Select 64 blocks from candidate blocks to generate stake modifier
    uint64_t nStakeModifierNew = 0;
    int64_t nSelectionIntervalStop = nSelectionIntervalStart;
    std::map<uint256, const CBlockIndex*> mapSelectedBlocks;
    for (int nRound = 0; nRound < std::min(64, (int)vSortedByTimestamp.size()); nRound++) {
        // add an interval section to the current selection round
        nSelectionIntervalStop += GetStakeModifierSelectionIntervalSection(nRound);

        // select a block from the candidates of current round
        if (!SelectBlockFromCandidates(vSortedByTimestamp, mapSelectedBlocks, nSelectionIntervalStop, nStakeModifier, &pindex))
            return error("%s : unable to select block at round %d", __func__, nRound);

        // write the entropy bit of the selected block
        nStakeModifierNew |= (((uint64_t)pindex->GetStakeEntropyBit()) << nRound);

        // add the selected block from candidates to selected list
        mapSelectedBlocks.insert(std::make_pair(pindex->GetBlockHash(), pindex));
        if (GetBoolArg("-printstakemodifier", false))
            LogPrintf("%s : selected round %d stop=%s height=%d bit=%d\n", __func__,
                nRound, DateTimeStrFormat("%Y-%m-%d %H:%M:%S", nSelectionIntervalStop).c_str(), pindex->nHeight, pindex->GetStakeEntropyBit());
    }

    // Print selection map for visualization of the selected blocks
    if (GetBoolArg("-printstakemodifier", false)) {
        std::string strSelectionMap = "";
        // '-' indicates proof-of-work blocks not selected
        strSelectionMap.insert(0, pindexPrev->nHeight - nHeightFirstCandidate + 1, '-');
        pindex = pindexPrev;
        while (pindex && pindex->nHeight >= nHeightFirstCandidate) {
            // '=' indicates proof-of-stake blocks not selected
            if (pindex->IsProofOfStake())
                strSelectionMap.replace(pindex->nHeight - nHeightFirstCandidate, 1, "=");
            pindex = pindex->pprev;
        }
        for (const std::pair<const uint256, const CBlockIndex*> &item : mapSelectedBlocks) {
            // 'S' indicates selected proof-of-stake blocks
            // 'W' indicates selected proof-of-work blocks
            strSelectionMap.replace(item.second->nHeight - nHeightFirstCandidate, 1, item.second->IsProofOfStake() ? "S" : "W");
        }
        LogPrintf("%s : selection height [%d, %d] map %s\n", __func__, nHeightFirstCandidate, pindexPrev->nHeight, strSelectionMap.c_str());
    }
    if (GetBoolArg("-printstakemodifier", false)) {
        LogPrintf("%s : new modifier=%s time=%s\n", __func__, std::to_string(nStakeModifierNew).c_str(), DateTimeStrFormat("%Y-%m-%d %H:%M:%S", pindexPrev->GetBlockTime()).c_str());
    }

    nStakeModifier = nStakeModifierNew;
    fGeneratedStakeModifier = true;

    return true;
}

// The stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
bool GetKernelStakeModifier(uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake)
{
    nStakeModifier = 0;
    if (!mapBlockIndex.count(hashBlockFrom))
        return error("%s : block not indexed", __func__);
    const CBlockIndex* pindexFrom = mapBlockIndex[hashBlockFrom];
    nStakeModifierHeight = pindexFrom->nHeight;
    nStakeModifierTime = pindexFrom->GetBlockTime();
    // Fixed stake modifier only for regtest
    if (Params().NetworkIDString() == CBaseChainParams::REGTEST) {
        nStakeModifier = pindexFrom->nStakeModifier;
        return true;
    }
    const CBlockIndex* pindex = pindexFrom;
    CBlockIndex* pindexNext = chainActive[pindex->nHeight + 1];;

    // loop to find the stake modifier later by a selection interval
    do {
        if (!pindexNext) {
            // Should never happen
            return error("%s : Null pindexNext, current block %s ", __func__, pindex->phashBlock->GetHex());
        }
        pindex = pindexNext;
        if (pindex->GeneratedStakeModifier()) {
            nStakeModifierHeight = pindex->nHeight;
            nStakeModifierTime = pindex->GetBlockTime();
        }
        pindexNext = chainActive[pindex->nHeight + 1];
    } while (nStakeModifierTime < pindexFrom->GetBlockTime() + OLD_MODIFIER_INTERVAL);

    nStakeModifier = pindex->nStakeModifier;

    return true;
}

bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, const unsigned int nBits, CStakeInput* stake, const unsigned int nTimeTx, uint256& hashProofOfStake, const bool fVerify)
{
    // Calculate the proof of stake hash
    if (!GetHashProofOfStake(pindexPrev, stake, nTimeTx, fVerify, hashProofOfStake)) {
        return error("%s : Failed to calculate the proof of stake hash", __func__);
    }

    const CAmount& nValueIn = stake->GetValue();
    const CDataStream& ssUniqueID = stake->GetUniqueness();

    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Weighted target
    arith_uint256 bnWeight = arith_uint256(nValueIn) / 100;
    bnTarget *= bnWeight;

    // Check if proof-of-stake hash meets target protocol
    const bool res = (UintToArith256(hashProofOfStake) < bnTarget);

    if (fVerify || res) {
        LogPrint("staking", "%s : Proof Of Stake:"
                            "\nssUniqueID=%s"
                            "\nnTimeTx=%d"
                            "\nhashProofOfStake=%s"
                            "\nnBits=%d"
                            "\nweight=%d"
                            "\nbnTarget=%s (res: %d)\n\n",
            __func__, HexStr(ssUniqueID), nTimeTx, hashProofOfStake.GetHex(),
            nBits, nValueIn, bnTarget.GetHex(), res);
    }

    return res;
}

bool GetHashProofOfStake(const CBlockIndex* pindexPrev, CStakeInput* stake, const unsigned int nTimeTx, const bool fVerify, uint256& hashProofOfStakeRet) {
    // Grab the stake data
    CBlockIndex* pindexfrom = stake->GetIndexFrom();
    if (!pindexfrom) return error("%s : Failed to find the block index for stake origin", __func__);
    const CDataStream& ssUniqueID = stake->GetUniqueness();
    const unsigned int nTimeBlockFrom = pindexfrom->nTime;
    CDataStream modifier_ss(SER_GETHASH, 0);

    // Hash the modifier
    // Modifier v2
    modifier_ss << pindexPrev->nStakeModifierV2;

    CDataStream ss(modifier_ss);
    // Calculate hash
    ss << nTimeBlockFrom << ssUniqueID << nTimeTx;
    hashProofOfStakeRet = Hash(ss.begin(), ss.end());

    if (fVerify) {
        LogPrint("staking", "%s :{ nStakeModifier=%s\n"
                            "}\n",
            __func__, HexStr(modifier_ss));
    }
    return true;
}

bool Stake(const CBlockIndex* pindexPrev, CStakeInput* stakeInput, unsigned int nBits, unsigned int& nTimeTx, uint256& hashProofOfStake)
{
    int prevHeight = pindexPrev->nHeight;

    // get stake input pindex
    CBlockIndex* pindexFrom = stakeInput->GetIndexFrom();
    if (!pindexFrom || pindexFrom->nHeight < 1) return error("%s : no pindexfrom", __func__);

    const uint32_t nTimeBlockFrom = pindexFrom->nTime;
    const int nHeightBlockFrom = pindexFrom->nHeight;

    // check for maturity (min age/depth) requirements
    if (!Params().HasStakeMinAgeOrDepth(prevHeight + 1, nTimeTx, nHeightBlockFrom, nTimeBlockFrom))
        return error("%s : min depth violation - height=%d - nTimeTx=%d, nTimeBlockFrom=%d, nHeightBlockFrom=%d",
                         __func__, prevHeight + 1, nTimeTx, nTimeBlockFrom, nHeightBlockFrom);

    // check for maturity (min age/depth) requirements
    if (Params().GetConsensus().nStakeMinAge > GetAdjustedTime() - nTimeBlockFrom)
        return error("%s : min age violation - height=%d - nTimeTx=%d, nTimeBlockFrom=%d, nHeightBlockFrom=%d",
                         __func__, prevHeight + 1, nTimeTx, nTimeBlockFrom, nHeightBlockFrom);

    // iterate the hashing
    bool fSuccess = false;
    const unsigned int nHashDrift = 60;
    unsigned int nTryTime = nTimeTx - 1;
    // iterate from nTimeTx up to nTimeTx + nHashDrift
    // but not after the max allowed future blocktime drift (3 minutes for PoS)
    const unsigned int maxTime = std::min(nTimeTx + nHashDrift, Params().MaxFutureBlockTime(GetAdjustedTime(), true));

    while (nTryTime < maxTime)
    {
        //new block came in, move on
        if (chainActive.Height() != prevHeight)
            break;

        ++nTryTime;

        // if stake hash does not meet the target then continue to next iteration
        if (!CheckStakeKernelHash(pindexPrev, nBits, stakeInput, nTryTime, hashProofOfStake))
            continue;

        // if we made it this far, then we have successfully found a valid kernel hash
        fSuccess = true;
        nTimeTx = nTryTime;
        break;
    }

    mapHashedBlocks.clear();
    mapHashedBlocks[chainActive.Tip()->nHeight] = GetTime(); //store a time stamp of when we last hashed on this block
    return fSuccess;
}

bool initStakeInput(const CBlock block, std::unique_ptr<CStakeInput>& stake, int nPreviousBlockHeight) {
    CTransactionRef ptx = block.vtx[1];
    if (!ptx->IsCoinStake())
        return error("%s : called on non-coinstake %s", __func__, ptx->GetHash().ToString().c_str());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = ptx->vin[0];

    //Construct the stakeinput object
    // First try finding the previous transaction in database
    uint256 hashBlock;
    CTransactionRef ptxPrev;
    if (!GetTransaction(txin.prevout.hash, ptxPrev, Params().GetConsensus(), hashBlock, true))
        return error("%s : INFO: read txPrev failed, tx id prev: %s, block id %s",
                     __func__, txin.prevout.hash.GetHex(), block.GetHash().GetHex());

    //verify signature and script
    if (!VerifyScript(txin.scriptSig, ptxPrev->vout[txin.prevout.n].scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS, TransactionSignatureChecker(ptx.get(), 0)))
        return error("%s : VerifySignature failed on coinstake %s", __func__, ptx->GetHash().ToString().c_str());

    const CTransaction& txPrev = *ptxPrev.get();
    CDynamicStake* stakeInput = new CDynamicStake();
    stakeInput->SetInput(txPrev, txin.prevout.n);
    stake = std::unique_ptr<CStakeInput>(stakeInput);

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(const CBlock& block, uint256& hashProofOfStake, std::unique_ptr<CStakeInput>& stake, const int& nPreviousBlockHeight)
{
    // Initialize the stake object
    if(!initStakeInput(block, stake, nPreviousBlockHeight))
        return error("%s : stake input object initialization failed", __func__);

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    CBlockIndex* pindexPrev = mapBlockIndex[block.hashPrevBlock];
    CBlockIndex* pindexfrom = stake->GetIndexFrom();
    if (!pindexfrom)
        return error("%s : Failed to find the block index for stake origin", __func__);

    unsigned int nTxTime = block.nTime;

    if (!CheckStakeKernelHash(pindexPrev, block.nBits, stake.get(), nTxTime, hashProofOfStake, true))
        return error("%s : INFO: check kernel failed on coinstake %s, hashProof=%s", __func__,
                     block.vtx[1]->GetHash().GetHex(), hashProofOfStake.GetHex());

    return true;
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    // v0.3 protocol
    return (nTimeBlock == nTimeTx);
}

// Check stake modifier hard checkpoints
bool CheckStakeModifierCheckpoints(int nHeight, unsigned int nStakeModifierChecksum)
{
    if (Params().NetworkIDString() != CBaseChainParams::MAIN) return true; // Testnet has no checkpoints
    if (mapStakeModifierCheckpoints.count(nHeight)) {
        return nStakeModifierChecksum == mapStakeModifierCheckpoints[nHeight];
    }
    return true;
}
