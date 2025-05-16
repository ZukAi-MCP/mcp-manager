// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "richlistdb.h"

#include "main.h"
#include "base58.h"

#include <vector>

#include <boost/version.hpp>
#include <boost/filesystem.hpp>

const string EIASaddresses[10] = {"BEaZDZ8gCbbP1y3t2gPNKwqZa76rUDfR73",
                                  "BDwfNiAvpb4ipNfPkdAXcPGExWHeeMdRcK",
                                  "BPVuYwyeJiXExEmEXHfCwPtRXDRqxBxTNW",
                                  "B4gB18iZWZ8nTuAvi9kq9cWbavCj6xSmny",
                                  "BGFEYswtWfo5nrKRT53ToGwZWyuRvwC8xs",
                                  "B7WWgP1fnPDHTL2z9vSHTpGqptP53t1UCB",
                                  "BEtL36SgjYuxHuU5dg8omJUAyTXDQL3Z8V",
                                  "BQaNeMcSyrzGkeKknjw6fnCSSLUYAsXCVd",
                                  "BDLAaqqtBNoG9EjbJCeuLSmT5wkdnSB8bc",
                                  "BQTar7kTE2hu4f4LrRmomjkbsqSW9rbMvy"};

inline int Height(const CAddressInfo &ai) {return ai.second;}
inline int Balance(const CAddressInfo &ai) {return ai.first;}

bool CRichList::SetForked(const bool &fFork)
{
    fForked = fFork;
    return true;
}

bool CRichList::NextRichScriptPubKey(CScript &scriptpubkey)
{
    if(maddresses.empty())
        return false;
    int minheight = 0;
    bool fFirst = true;    
    for(mapScriptPubKeys::const_iterator it = maddresses.begin(); it != maddresses.end(); it++)
    {
        if(fFirst || it->second.second <= minheight) {
            scriptpubkey = it->first;
            minheight = it->second.second;
            fFirst = false;         
        }
    }
    return true;
}

CScript CRichList::NextEIASScriptPubKey(const int &nHeight){
    CScript ret;
    ret.SetDestination(CBitcoinAddress(EIASaddresses[nHeight % 10]).Get());
    return ret;
}

bool CRichList::UpdateAddressInfo(const std::map<CScript, std::pair<int64_t, int> > &map)
{
    for(std::map<CScript, std::pair<int64_t, int> >::const_iterator it = map.begin(); it!= map.end(); it++)
    {
        mapScriptPubKeys::iterator itRich = maddresses.find(it->first);
        if(itRich!=maddresses.end()){

            if(it->second.first < RICH_AMOUNT) {
                maddresses.erase(itRich);
            } else { 
                itRich->second = it->second;
            }
        }
        else if(it->second.first >= RICH_AMOUNT)
            maddresses.insert(*it);
    }
    return true;
}

// The heights need to be rolled back before new blocks are connected if any were disconnected.
// TODO: We should try to get rid of this and write the height undo information to the disk instead.
bool CRichList::UpdateRichAddressHeights()
{
    if(!fForked) 
        return true;

    CBlockIndex* pindexSeek = mapBlockIndex.find(pcoinsTip->GetBestBlock())->second;
    if(!chainActive.Contains(pindexSeek))
    	return false;
    CBlock block;
    std::map<CScript, std::pair<int64_t, int> > addressInfo;
    mapScriptPubKeys mforkedAddresses;

    for(mapScriptPubKeys::const_iterator it = maddresses.begin(); it!=maddresses.end(); it++)
    {
        if(it->second.second > pindexSeek->nHeight)
            mforkedAddresses.insert(*it);
    }

    if(fDebug) {
        LogPrintf("%d addresses seen at fork and need to be relocated\n", mforkedAddresses.size());
    }
    // assert(!"UpdateRichAddressHeights(): Deliberate failure");

    while(pindexSeek->pprev && !mforkedAddresses.empty())
    {       

        // return false;

        
        ReadBlockFromDisk(block,pindexSeek);
        block.BuildMerkleTree();
        BOOST_FOREACH(const CTransaction &tx, block.vtx)
        {
            for(unsigned int j = 0; j < tx.vout.size(); j++)
            {
                CScript key = tx.vout[j].scriptPubKey;
                mapScriptPubKeys::iterator it = mforkedAddresses.find(key);
                if(it == mforkedAddresses.end())
                    continue;
                if(pindexSeek->nHeight < nRichForkV2Height 
                    || tx.IsCoinBase()
                    || Balance(it) - tx.vout[j].nValue < RICH_AMOUNT) // if it isn't currently rich (wrt pindexSeek) but was once the output will be caught first in the undo block
                {
                    addressInfo.insert(std::make_pair(key, std::make_pair(Balance(it), pindexSeek->nHeight)));
                    mforkedAddresses.erase(it);
                    if(fDebug) {
                        CTxDestination dest;
                        ExtractDestination(key, dest);
                        CBitcoinAddress addr;
                        addr.Set(dest);
                        LogPrintf("%s found at height %d\n",addr.ToString(),pindexSeek->nHeight);  
                    }                              
                }
            }
        }

        CBlockUndo undo;
        CDiskBlockPos pos = pindexSeek ->GetUndoPos();
        if (undo.ReadFromDisk(pos, pindexSeek->pprev->GetBlockHash())) //TODO: hvenær klikkar þetta?
        {
            for (unsigned int i=0; i<undo.vtxundo.size(); i++)
            {
                for (unsigned int j=0; j<undo.vtxundo[i].vprevout.size(); j++)
                {
                    CScript key = undo.vtxundo[i].vprevout[j].txout.scriptPubKey;
                    mapScriptPubKeys::iterator it = mforkedAddresses.find(key);
                    if(it == mforkedAddresses.end())
                        continue;                        
                    addressInfo.insert(std::make_pair(it->first, std::make_pair(Balance(it), pindexSeek->nHeight)));
                    mforkedAddresses.erase(it);
                    if(fDebug) {
                        CTxDestination dest;
                        ExtractDestination(key, dest);
                        CBitcoinAddress addr;
                        addr.Set(dest);
                        LogPrintf("%s found at height %d\n",addr.ToString(),pindexSeek->nHeight);  
                    }              
                }
            }
        }
        else {
            LogPrintf("UpdateRichAddressHeights(): Failed to read undo information\n");
            break;
        }
        pindexSeek = pindexSeek -> pprev;
    }
	
	bool ret;
	typedef std::pair<CScript,std::pair<int64_t,int> > pairType;
	BOOST_FOREACH(const pairType &pair, addressInfo) {
		ret = pcoinsTip->SetAddressInfo(pair.first, pair.second);
		assert(ret);
	}

    if(!UpdateAddressInfo(addressInfo))
        return false;
    return mforkedAddresses.empty();
}

bool CRichList::GetRichAddresses(std::multiset< std::pair< CScript, std::pair<int64_t, int> >,RichOrderCompare > &retset) const{
    for(std::map<CScript, std::pair<int64_t, int> >::const_iterator it=maddresses.begin(); it!=maddresses.end(); it++)
    {
        if(it->second.first>=RICH_AMOUNT)
        {
            retset.insert(std::make_pair(it->first, std::make_pair(it->second.first,it->second.second)));
        }
    }
    return true;
}