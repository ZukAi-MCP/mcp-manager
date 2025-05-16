// Copyright (c) 2012-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "coins.h"
#include "base58.h"

#include <assert.h>

//Calculate number of bytes for the bitmask, and its number of non-zero bytes
void CCoins::CalcMaskSize(unsigned int &nBytes, unsigned int &nNonzeroBytes) const {
    unsigned int nLastUsedByte = 0;
    for (unsigned int b = 0; 2+b*8 < vout.size(); b++) {
        bool fZero = true;
        for (unsigned int i = 0; i < 8 && 2+b*8+i < vout.size(); i++) {
            if (!vout[2+b*8+i].IsNull()) {
                fZero = false;
                continue;
            }
        }
        if (!fZero) {
            nLastUsedByte = b + 1;
            nNonzeroBytes++;
        }
    }
    nBytes += nLastUsedByte;
}

bool CCoins::Spend(const COutPoint &out, CTxInUndo &undo) {
    if (out.n >= vout.size())
        return false;
    if (vout[out.n].IsNull())
        return false;
    undo = CTxInUndo(vout[out.n]);
    vout[out.n].SetNull();
    Cleanup();
    if (vout.size() == 0) {
        undo.nHeight = nHeight;
        undo.fCoinBase = fCoinBase;
        undo.nVersion = this->nVersion;
    }
    return true;
}

bool CCoins::Spend(int nPos) {
    CTxInUndo undo;
    COutPoint out(0, nPos);
    return Spend(out, undo);
}


bool CCoinsView::GetCoins(const uint256 &txid, CCoins &coins) { return false; }
bool CCoinsView::SetCoins(const uint256 &txid, const CCoins &coins) { return false; }
bool CCoinsView::GetAddressInfo(const CScript &key, std::pair<int64_t,int> &value) { return false; }
bool CCoinsView::SetAddressInfo(const CScript &key, const std::pair<int64_t,int> &value) { return false; }
bool CCoinsView::GetServiceInfo(const std::string &key, std::tuple<std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::SetServiceInfo(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::GetCouponList(const std::string &key, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::SetCouponList(const std::string &key, const std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::GetUbiList(const std::string &key, std::tuple<std::string, std::string> &value) { return false; }
bool CCoinsView::SetUbiList(const std::string &key, const std::tuple<std::string, std::string> &value) { return false; }
bool CCoinsView::GetDexList(const std::string &key, std::tuple<std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::SetDexList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::GetBookList(const std::string &key, std::tuple<std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::SetBookList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::GetNPList(const std::string &key, std::tuple<std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::SetNPList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) { return false; }
bool CCoinsView::HaveCoins(const uint256 &txid) { return false; }
uint256 CCoinsView::GetBestBlock() { return uint256(0); }
bool CCoinsView::SetBestBlock(const uint256 &hashBlock) { return false; }
bool CCoinsView::BatchWrite(const std::map<uint256, CCoins> &mapCoins, const std::map<CScript, std::pair<int64_t,int> > &mapAddressInfo,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceInfo,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> > &mapServiceCouponList,
                            const std::map<std::string, std::tuple<std::string, std::string> > &mapServiceUbiList,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceDexList,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceBookList,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceNPList,
                            const uint256 &hashBlock) { return false; }
bool CCoinsView::GetStats(CCoinsStats &stats) { return false; }


CCoinsViewBacked::CCoinsViewBacked(CCoinsView &viewIn) : base(&viewIn) { }
bool CCoinsViewBacked::GetCoins(const uint256 &txid, CCoins &coins) { return base->GetCoins(txid, coins); }
bool CCoinsViewBacked::SetCoins(const uint256 &txid, const CCoins &coins) { return base->SetCoins(txid, coins); }
bool CCoinsViewBacked::GetAddressInfo(const CScript &key, std::pair<int64_t,int> &value) { return base->GetAddressInfo(key, value); }
bool CCoinsViewBacked::SetAddressInfo(const CScript &key, const std::pair<int64_t,int> &value) { return base->SetAddressInfo(key, value); }
bool CCoinsViewBacked::GetServiceInfo(const std::string &key, std::tuple<std::string, std::string, std::string> &value) { return base->GetServiceInfo(key, value); }
bool CCoinsViewBacked::SetServiceInfo(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) { return base->SetServiceInfo(key, value); }
bool CCoinsViewBacked::GetCouponList(const std::string &key, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value) { return base->GetCouponList(key, value); }
bool CCoinsViewBacked::SetCouponList(const std::string &key, const std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value) { return base->SetCouponList(key, value); }
bool CCoinsViewBacked::GetUbiList(const std::string &key, std::tuple<std::string, std::string> &value) { return base->GetUbiList(key, value); }
bool CCoinsViewBacked::SetUbiList(const std::string &key, const std::tuple<std::string, std::string> &value) { return base->SetUbiList(key, value); }
bool CCoinsViewBacked::GetDexList(const std::string &key, std::tuple<std::string, std::string, std::string> &value) { return base->GetDexList(key, value); }
bool CCoinsViewBacked::SetDexList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) { return base->SetDexList(key, value); }
bool CCoinsViewBacked::GetBookList(const std::string &key, std::tuple<std::string, std::string, std::string> &value) { return base->GetBookList(key, value); }
bool CCoinsViewBacked::SetBookList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) { return base->SetBookList(key, value); }
bool CCoinsViewBacked::GetNPList(const std::string &key, std::tuple<std::string, std::string, std::string> &value) { return base->GetNPList(key, value); }
bool CCoinsViewBacked::SetNPList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) { return base->SetNPList(key, value); }
bool CCoinsViewBacked::HaveCoins(const uint256 &txid) { return base->HaveCoins(txid); }
uint256 CCoinsViewBacked::GetBestBlock() { return base->GetBestBlock(); }
bool CCoinsViewBacked::SetBestBlock(const uint256 &hashBlock) { return base->SetBestBlock(hashBlock); }
void CCoinsViewBacked::SetBackend(CCoinsView &viewIn) { base = &viewIn; }
bool CCoinsViewBacked::BatchWrite(const std::map<uint256, CCoins> &mapCoins, const std::map<CScript, std::pair<int64_t,int> > &mapAddressInfo,
                                  const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceInfo,
                                  const std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> > &mapServiceCouponList,
                                  const std::map<std::string, std::tuple<std::string, std::string> > &mapServiceUbiList,
                                  const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceDexList,
                                  const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceBookList,
                                  const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceNPList,
                                  const uint256 &hashBlock) { return base->BatchWrite(mapCoins, mapAddressInfo, mapServiceInfo, mapServiceCouponList, mapServiceUbiList, mapServiceDexList, mapServiceBookList, mapServiceNPList, hashBlock); }
bool CCoinsViewBacked::GetStats(CCoinsStats &stats) { return base->GetStats(stats); }

CCoinsViewCache::CCoinsViewCache(CCoinsView &baseIn, bool fDummy) : CCoinsViewBacked(baseIn), hashBlock(0) { }

bool CCoinsViewCache::GetCoins(const uint256 &txid, CCoins &coins) {
    if (cacheCoins.count(txid)) {
        coins = cacheCoins[txid];
        return true;
    }
    if (base->GetCoins(txid, coins)) {
        cacheCoins[txid] = coins;
        return true;
    }
    return false;
}

bool CCoinsViewCache::GetAddressInfo(const CScript &key, std::pair<int64_t,int> &value) {
    std::map<CScript,std::pair<int64_t,int> >::iterator it = cacheAddressInfo.find(key);
    if(it!=cacheAddressInfo.end()) {
        value = it->second;
        return true;
    }
    if(base->GetAddressInfo(key,value)) {
        cacheAddressInfo[key] = value;
        return true;
    }
    return false;
}

bool CCoinsViewCache::SetAddressInfo(const CScript &key, const std::pair<int64_t,int> &value) {
    cacheAddressInfo[key] = value;
    return true;
}

bool CCoinsViewCache::GetServiceInfo(const std::string &key, std::tuple<std::string, std::string, std::string> &value) {
    std::map<std::string, std::tuple<std::string, std::string, std::string> >::iterator it = cacheServiceInfo.find(key);
    if(it!=cacheServiceInfo.end()) {
        value = it->second;
        return true;
    }
    if(base->GetServiceInfo(key,value)) {
        cacheServiceInfo[key] = value;
        return true;
    }
    return false;
}

bool CCoinsViewCache::SetServiceInfo(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) {
    cacheServiceInfo[key] = value;
    return true;
}

bool CCoinsViewCache::GetCouponList(const std::string &key, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value) {
    std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> >::iterator it = cacheServiceCouponList.find(key);

    if(it!=cacheServiceCouponList.end()) {
        value = it->second;
        return true;
    }
    if(base->GetCouponList(key,value)) {
        cacheServiceCouponList[key] = value;
        return true;
    }
    return false;
}

bool CCoinsViewCache::SetCouponList(const std::string &key, const std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value) {
    cacheServiceCouponList[key] = value;
    return true;
}

bool CCoinsViewCache::GetUbiList(const std::string &key, std::tuple<std::string, std::string> &value) {
    std::map<std::string, std::tuple<std::string, std::string> >::iterator it = cacheServiceUbiList.find(key);

    if(it!=cacheServiceUbiList.end()) {
        value = it->second;
        return true;
    }
    if(base->GetUbiList(key,value)) {
        cacheServiceUbiList[key] = value;
        return true;
    }
    return false;
}

bool CCoinsViewCache::SetUbiList(const std::string &key, const std::tuple<std::string, std::string> &value) {
    cacheServiceUbiList[key] = value;
    return true;
}

bool CCoinsViewCache::GetDexList(const std::string &key, std::tuple<std::string, std::string, std::string> &value) {
    std::map<std::string, std::tuple<std::string, std::string, std::string> >::iterator it = cacheServiceDexList.find(key);

    if(it!=cacheServiceDexList.end()) {
        value = it->second;
        return true;
    }
    if(base->GetDexList(key,value)) {
        cacheServiceDexList[key] = value;
        return true;
    }
    return false;
}

bool CCoinsViewCache::SetDexList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) {
    cacheServiceDexList[key] = value;
    return true;
}

bool CCoinsViewCache::GetBookList(const std::string &key, std::tuple<std::string, std::string, std::string> &value) {
    std::map<std::string, std::tuple<std::string, std::string, std::string> >::iterator it = cacheServiceBookList.find(key);

    if(it!=cacheServiceBookList.end()) {
        value = it->second;
        return true;
    }
    if(base->GetBookList(key,value)) {
        cacheServiceBookList[key] = value;
        return true;
    }
    return false;
}

bool CCoinsViewCache::SetBookList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) {
    cacheServiceBookList[key] = value;
    return true;
}

bool CCoinsViewCache::GetNPList(const std::string &key, std::tuple<std::string, std::string, std::string> &value) {
    std::map<std::string, std::tuple<std::string, std::string, std::string> >::iterator it = cacheServiceNPList.find(key);

    if(it!=cacheServiceNPList.end()) {
        value = it->second;
        return true;
    }
    if(base->GetNPList(key,value)) {
        cacheServiceNPList[key] = value;
        return true;
    }
    return false;
}

bool CCoinsViewCache::SetNPList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value) {
    cacheServiceNPList[key] = value;
    return true;
}

std::map<uint256,CCoins>::iterator CCoinsViewCache::FetchCoins(const uint256 &txid) {
    std::map<uint256,CCoins>::iterator it = cacheCoins.lower_bound(txid);
    if (it != cacheCoins.end() && it->first == txid)
        return it;
    CCoins tmp;
    if (!base->GetCoins(txid,tmp))
        return cacheCoins.end();
    std::map<uint256,CCoins>::iterator ret = cacheCoins.insert(it, std::make_pair(txid, CCoins()));
    tmp.swap(ret->second);
    return ret;
}

CCoins &CCoinsViewCache::GetCoins(const uint256 &txid) {
    std::map<uint256,CCoins>::iterator it = FetchCoins(txid);
    assert(it != cacheCoins.end());
    return it->second;
}

bool CCoinsViewCache::SetCoins(const uint256 &txid, const CCoins &coins) {
    cacheCoins[txid] = coins;
    return true;
}

bool CCoinsViewCache::HaveCoins(const uint256 &txid) {
    return FetchCoins(txid) != cacheCoins.end();
}

uint256 CCoinsViewCache::GetBestBlock() {
    if (hashBlock == uint256(0))
        hashBlock = base->GetBestBlock();
    return hashBlock;
}

bool CCoinsViewCache::SetBestBlock(const uint256 &hashBlockIn) {
    hashBlock = hashBlockIn;
    return true;
}

bool CCoinsViewCache::BatchWrite(const std::map<uint256, CCoins> &mapCoins, 
                                 const std::map<CScript, std::pair<int64_t,int> > &mapAddressInfo,
                                 const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceInfo,
                                 const std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> > &mapServiceCouponList,
                                 const std::map<std::string, std::tuple<std::string, std::string> > &mapServiceUbiList,
                                 const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceDexList,
                                 const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceBookList,
                                 const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceNPList,
                                 const uint256 &hashBlockIn) {
    for (std::map<uint256, CCoins>::const_iterator it = mapCoins.begin(); it != mapCoins.end(); it++)
        cacheCoins[it->first] = it->second;
    for (std::map<CScript, std::pair<int64_t,int> >::const_iterator it = mapAddressInfo.begin(); it != mapAddressInfo.end(); it++)
        cacheAddressInfo[it->first] = it->second;
    for (std::map<std::string, std::tuple<std::string, std::string, std::string> >::const_iterator it = mapServiceInfo.begin(); it != mapServiceInfo.end(); it++)
        cacheServiceInfo[it->first] = it->second;
    for (std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> >::const_iterator it = mapServiceCouponList.begin(); it != mapServiceCouponList.end(); it++)
        cacheServiceCouponList[it->first] = it->second;
    for (std::map<std::string, std::tuple<std::string, std::string> >::const_iterator it = mapServiceUbiList.begin(); it != mapServiceUbiList.end(); it++)
        cacheServiceUbiList[it->first] = it->second;
    for (std::map<std::string, std::tuple<std::string, std::string, std::string> >::const_iterator it = mapServiceDexList.begin(); it != mapServiceDexList.end(); it++)
        cacheServiceDexList[it->first] = it->second;
    for (std::map<std::string, std::tuple<std::string, std::string, std::string> >::const_iterator it = mapServiceBookList.begin(); it != mapServiceBookList.end(); it++)
        cacheServiceBookList[it->first] = it->second;
    for (std::map<std::string, std::tuple<std::string, std::string, std::string> >::const_iterator it = mapServiceNPList.begin(); it != mapServiceNPList.end(); it++)
        cacheServiceNPList[it->first] = it->second;

    hashBlock = hashBlockIn;
    return true;
}

bool CCoinsViewCache::Flush() {
    bool fOk = base->BatchWrite(cacheCoins, cacheAddressInfo, cacheServiceInfo, cacheServiceCouponList, cacheServiceUbiList, cacheServiceDexList, cacheServiceBookList, cacheServiceNPList, hashBlock);
    if (fOk) {
        cacheCoins.clear();
        cacheAddressInfo.clear();
        cacheServiceInfo.clear();
        cacheServiceCouponList.clear();
        cacheServiceUbiList.clear();
        cacheServiceDexList.clear();
        cacheServiceBookList.clear();
        cacheServiceNPList.clear();
    }
    return fOk;
}

unsigned int CCoinsViewCache::GetCacheSize() {
    return cacheCoins.size();
}

const CTxOut &CCoinsViewCache::GetOutputFor(const CTxIn& input)
{
    const CCoins &coins = GetCoins(input.prevout.hash);
    assert(coins.IsAvailable(input.prevout.n));
    return coins.vout[input.prevout.n];
}

int64_t CCoinsViewCache::GetValueIn(const CTransaction& tx)
{
    if (tx.IsCoinBase())
        return 0;

    int64_t nResult = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
        nResult += GetOutputFor(tx.vin[i]).nValue;

    return nResult;
}

bool CCoinsViewCache::HaveInputs(const CTransaction& tx)
{
    if (!tx.IsCoinBase()) {
        // first check whether information about the prevout hash is available
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            const COutPoint &prevout = tx.vin[i].prevout;
            if (!HaveCoins(prevout.hash))
                return false;
        }

        // then check whether the actual outputs are available
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            const COutPoint &prevout = tx.vin[i].prevout;
            const CCoins &coins = GetCoins(prevout.hash);
            if (!coins.IsAvailable(prevout.n))
                return false;
        }
    }
    return true;
}

double CCoinsViewCache::GetPriority(const CTransaction &tx, int nHeight)
{
    if (tx.IsCoinBase())
        return 0.0;
    double dResult = 0.0;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        const CCoins &coins = GetCoins(txin.prevout.hash);
        if (!coins.IsAvailable(txin.prevout.n)) continue;
        if (coins.nHeight < nHeight) {
            dResult += coins.vout[txin.prevout.n].nValue * (nHeight-coins.nHeight);
        }
    }
    return tx.ComputePriority(dResult);
}
