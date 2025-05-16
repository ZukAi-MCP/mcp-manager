// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_COINS_H
#define BITCOIN_COINS_H

#include "core.h"
#include "serialize.h"
#include "uint256.h"

#include <assert.h>
#include <stdint.h>

#include <boost/foreach.hpp>

//Pruned version of CTransaction: only retains metadata and unspent transaction outputs
class CCoins
{
public:
    //Whether transaction is a coinbase
    bool fCoinBase;

    //Unspent transaction outputs; spent outputs are .IsNull(); spent outputs at the end of the array are dropped
    std::vector<CTxOut> vout;

    //At which height this transaction was included in the active block chain
    int nHeight;

    //Version of the CTransaction
    int nVersion;

    //Construct a CCoins from a CTransaction, at a given height
    CCoins(const CTransaction &tx, int nHeightIn) : fCoinBase(tx.IsCoinBase()), vout(tx.vout), nHeight(nHeightIn), nVersion(tx.nVersion) {
        ClearUnspendable();
    }

    //Empty constructor
    CCoins() : fCoinBase(false), vout(0), nHeight(0), nVersion(0) { }

    //Remove spent outputs at the end of vout
    void Cleanup() {
        while (vout.size() > 0 && vout.back().IsNull())
            vout.pop_back();
        if (vout.empty())
            std::vector<CTxOut>().swap(vout);
    }

    void ClearUnspendable() {
        BOOST_FOREACH(CTxOut &txout, vout) {
            if (txout.scriptPubKey.IsUnspendable())
                txout.SetNull();
        }
        Cleanup();
    }

    void swap(CCoins &to) {
        std::swap(to.fCoinBase, fCoinBase);
        to.vout.swap(vout);
        std::swap(to.nHeight, nHeight);
        std::swap(to.nVersion, nVersion);
    }

    //Equality test
    friend bool operator==(const CCoins &a, const CCoins &b) {
         //Empty CCoins objects are always equal.
         if (a.IsPruned() && b.IsPruned())
             return true;
         return a.fCoinBase == b.fCoinBase &&
                a.nHeight == b.nHeight &&
                a.nVersion == b.nVersion &&
                a.vout == b.vout;
    }
    friend bool operator!=(const CCoins &a, const CCoins &b) {
        return !(a == b);
    }

    void CalcMaskSize(unsigned int &nBytes, unsigned int &nNonzeroBytes) const;

    bool IsCoinBase() const {
        return fCoinBase;
    }

    unsigned int GetSerializeSize(int nType, int nVersion) const {
        unsigned int nSize = 0;
        unsigned int nMaskSize = 0, nMaskCode = 0;
        CalcMaskSize(nMaskSize, nMaskCode);
        bool fFirst = vout.size() > 0 && !vout[0].IsNull();
        bool fSecond = vout.size() > 1 && !vout[1].IsNull();
        assert(fFirst || fSecond || nMaskCode);
        unsigned int nCode = 8*(nMaskCode - (fFirst || fSecond ? 0 : 1)) + (fCoinBase ? 1 : 0) + (fFirst ? 2 : 0) + (fSecond ? 4 : 0);
        //Version
        nSize += ::GetSerializeSize(VARINT(this->nVersion), nType, nVersion);
        //Size of header code
        nSize += ::GetSerializeSize(VARINT(nCode), nType, nVersion);
        //Spentness bitmask
        nSize += nMaskSize;
        //Txouts themself
        for (unsigned int i = 0; i < vout.size(); i++)
            if (!vout[i].IsNull())
                nSize += ::GetSerializeSize(CTxOutCompressor(REF(vout[i])), nType, nVersion);
        //Height
        nSize += ::GetSerializeSize(VARINT(nHeight), nType, nVersion);
        return nSize;
    }

    template<typename Stream>
    void Serialize(Stream &s, int nType, int nVersion) const {
        unsigned int nMaskSize = 0, nMaskCode = 0;
        CalcMaskSize(nMaskSize, nMaskCode);
        bool fFirst = vout.size() > 0 && !vout[0].IsNull();
        bool fSecond = vout.size() > 1 && !vout[1].IsNull();
        assert(fFirst || fSecond || nMaskCode);
        unsigned int nCode = 8*(nMaskCode - (fFirst || fSecond ? 0 : 1)) + (fCoinBase ? 1 : 0) + (fFirst ? 2 : 0) + (fSecond ? 4 : 0);
        //Version
        ::Serialize(s, VARINT(this->nVersion), nType, nVersion);
        //Header code
        ::Serialize(s, VARINT(nCode), nType, nVersion);
        //Spentness bitmask
        for (unsigned int b = 0; b<nMaskSize; b++) {
            unsigned char chAvail = 0;
            for (unsigned int i = 0; i < 8 && 2+b*8+i < vout.size(); i++)
                if (!vout[2+b*8+i].IsNull())
                    chAvail |= (1 << i);
            ::Serialize(s, chAvail, nType, nVersion);
        }
        //Txouts themself
        for (unsigned int i = 0; i < vout.size(); i++) {
            if (!vout[i].IsNull())
                ::Serialize(s, CTxOutCompressor(REF(vout[i])), nType, nVersion);
        }
        //Coinbase height
        ::Serialize(s, VARINT(nHeight), nType, nVersion);
    }

    template<typename Stream>
    void Unserialize(Stream &s, int nType, int nVersion) {
        unsigned int nCode = 0;
        //Version
        ::Unserialize(s, VARINT(this->nVersion), nType, nVersion);
        //Header code
        ::Unserialize(s, VARINT(nCode), nType, nVersion);
        fCoinBase = nCode & 1;
        std::vector<bool> vAvail(2, false);
        vAvail[0] = nCode & 2;
        vAvail[1] = nCode & 4;
        unsigned int nMaskCode = (nCode / 8) + ((nCode & 6) != 0 ? 0 : 1);
        //Spentness bitmask
        while (nMaskCode > 0) {
            unsigned char chAvail = 0;
            ::Unserialize(s, chAvail, nType, nVersion);
            for (unsigned int p = 0; p < 8; p++) {
                bool f = (chAvail & (1 << p)) != 0;
                vAvail.push_back(f);
            }
            if (chAvail != 0)
                nMaskCode--;
        }
        //Txouts themself
        vout.assign(vAvail.size(), CTxOut());
        for (unsigned int i = 0; i < vAvail.size(); i++) {
            if (vAvail[i])
                ::Unserialize(s, REF(CTxOutCompressor(vout[i])), nType, nVersion);
        }
        //Coinbase height
        ::Unserialize(s, VARINT(nHeight), nType, nVersion);
        Cleanup();
    }

    //Mark an outpoint spent, and construct undo information
    bool Spend(const COutPoint &out, CTxInUndo &undo);

    //Mark a vout spent
    bool Spend(int nPos);

    //Check whether a particular output is still available
    bool IsAvailable(unsigned int nPos) const {
        return (nPos < vout.size() && !vout[nPos].IsNull());
    }

    //Check whether the entire CCoins is spent. Only !IsPruned() CCoins can be serialized
    bool IsPruned() const {
        BOOST_FOREACH(const CTxOut &out, vout)
            if (!out.IsNull())
                return false;
        return true;
    }
};


struct CCoinsStats
{
    int nHeight;
    uint256 hashBlock;
    uint64_t nTransactions;
    uint64_t nTransactionOutputs;
    uint64_t nSerializedSize;
    uint256 hashSerialized;
    int64_t nTotalAmount;

    CCoinsStats() : nHeight(0), hashBlock(0), nTransactions(0), nTransactionOutputs(0), nSerializedSize(0), hashSerialized(0), nTotalAmount(0) {}
};


//Abstract view on the open txout dataset.
class CCoinsView
{
public:
    //Retrieve the CCoins (unspent transaction outputs) for a given txid
    virtual bool GetCoins(const uint256 &txid, CCoins &coins);

    //Modify the CCoins for a given txid
    virtual bool SetCoins(const uint256 &txid, const CCoins &coins);

    //Retrieve the balance of and height where a given scriptPubKey was last used.
    virtual bool GetAddressInfo(const CScript &key, std::pair<int64_t,int> &value);

    //Modify the balance of and height where a given scriptPubKey was last used.
    virtual bool SetAddressInfo(const CScript &key, const std::pair<int64_t,int> &value);

    //Retrieve the balance of and height where a given scriptPubKey was last used.
    virtual bool GetServiceInfo(const std::string &key, std::tuple<std::string, std::string, std::string> &value);

    //Modify the balance of and height where a given scriptPubKey was last used.
    virtual bool SetServiceInfo(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);

    //Retrieve the balance of and height where a given scriptPubKey was last used - servicecouponlist
    virtual bool GetCouponList(const std::string &key, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value);

    //Modify the balance of and height where a given scriptPubKey was last used - servicecouponlist
    virtual bool SetCouponList(const std::string &key, const std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value);

    //Retrieve the balance of and height where a given scriptPubKey was last used - serviceubilist
    virtual bool GetUbiList(const std::string &key, std::tuple<std::string, std::string> &value);

    //Modify the balance of and height where a given scriptPubKey was last used - serviceubilist
    virtual bool SetUbiList(const std::string &key, const std::tuple<std::string, std::string> &value);

    //Retrieve the balance of and height where a given scriptPubKey was last used - servicedexlist
    virtual bool GetDexList(const std::string &key, std::tuple<std::string, std::string, std::string> &value);

    //Modify the balance of and height where a given scriptPubKey was last used - servicedexlist
    virtual bool SetDexList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);

    //Retrieve the balance of and height where a given scriptPubKey was last used - servicebooklist
    virtual bool GetBookList(const std::string &key, std::tuple<std::string, std::string, std::string> &value);

    //Modify the balance of and height where a given scriptPubKey was last used - servicebooklist
    virtual bool SetBookList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);

    virtual bool GetNPList(const std::string &key, std::tuple<std::string, std::string, std::string> &value);

    //Modify the balance of and height where a given scriptPubKey was last used - servicebooklist
    virtual bool SetNPList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);

    //Just check whether we have data for a given txid.
    virtual bool HaveCoins(const uint256 &txid);

    //Retrieve the block hash whose state this CCoinsView currently represents
    virtual uint256 GetBestBlock();

    //Modify the currently active block hash
    virtual bool SetBestBlock(const uint256 &hashBlock);

    //Do a bulk modification (multiple SetCoins + one SetBestBlock)
    virtual bool BatchWrite(const std::map<uint256, CCoins> &mapCoins, const std::map<CScript, std::pair<int64_t,int> > &mapAddressInfo,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceInfo,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> > &mapServiceCouponList,
                            const std::map<std::string, std::tuple<std::string, std::string> > &mapServiceUbiList,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceDexList,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceBookList,
                            const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceNPList,
                            const uint256 &hashBlock);

    //Calculate statistics about the unspent transaction output set
    virtual bool GetStats(CCoinsStats &stats);

    //As we use CCoinsViews polymorphically, have a virtual destructor
    virtual ~CCoinsView() {}
};


//CCoinsView backed by another CCoinsView
class CCoinsViewBacked : public CCoinsView
{
protected:
    CCoinsView *base;

public:
    CCoinsViewBacked(CCoinsView &viewIn);
    bool GetCoins(const uint256 &txid, CCoins &coins);
    bool SetCoins(const uint256 &txid, const CCoins &coins);
    bool GetAddressInfo(const CScript &key, std::pair<int64_t,int> &value);
    bool SetAddressInfo(const CScript &key, const std::pair<int64_t,int> &value);
    bool GetServiceInfo(const std::string &key, std::tuple<std::string, std::string, std::string> &value);
    bool SetServiceInfo(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);
    bool GetCouponList(const std::string &key, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value);
    bool SetCouponList(const std::string &key, const std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value);
    bool GetUbiList(const std::string &key, std::tuple<std::string, std::string> &value);
    bool SetUbiList(const std::string &key, const std::tuple<std::string, std::string> &value);
    bool GetDexList(const std::string &key, std::tuple<std::string, std::string, std::string> &value);
    bool SetDexList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);
    bool GetBookList(const std::string &key, std::tuple<std::string, std::string, std::string> &value);
    bool SetBookList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);
    bool GetNPList(const std::string &key, std::tuple<std::string, std::string, std::string> &value);
    bool SetNPList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);
    bool HaveCoins(const uint256 &txid);
    uint256 GetBestBlock();
    bool SetBestBlock(const uint256 &hashBlock);
    void SetBackend(CCoinsView &viewIn);
    bool BatchWrite(const std::map<uint256, CCoins> &mapCoins, const std::map<CScript, std::pair<int64_t,int> > &mapAddressInfo,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceInfo,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> > &mapServiceCouponList,
                    const std::map<std::string, std::tuple<std::string, std::string> > &mapServiceUbiList,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceDexList,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceBookList,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceNPList,
                    const uint256 &hashBlock);
    bool GetStats(CCoinsStats &stats);
};


//CCoinsView that adds a memory cache for transactions to another CCoinsView
class CCoinsViewCache : public CCoinsViewBacked
{
protected:
    uint256 hashBlock;
    std::map<uint256,CCoins> cacheCoins;
    std::map<CScript, std::pair<int64_t,int> > cacheAddressInfo;
    std::map<std::string, std::tuple<std::string, std::string, std::string> > cacheServiceInfo;
    std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> > cacheServiceCouponList;
    std::map<std::string, std::tuple<std::string, std::string> > cacheServiceUbiList;
    std::map<std::string, std::tuple<std::string, std::string, std::string> > cacheServiceDexList;
    std::map<std::string, std::tuple<std::string, std::string, std::string> > cacheServiceBookList;
    std::map<std::string, std::tuple<std::string, std::string, std::string> > cacheServiceNPList;

public:
    CCoinsViewCache(CCoinsView &baseIn, bool fDummy = false);

    //Standard CCoinsView methods
    bool GetCoins(const uint256 &txid, CCoins &coins);
    bool SetCoins(const uint256 &txid, const CCoins &coins);
    bool GetAddressInfo(const CScript &key, std::pair<int64_t,int> &value);
    bool SetAddressInfo(const CScript &key, const std::pair<int64_t,int> &value);
    bool GetServiceInfo(const std::string &key, std::tuple<std::string, std::string, std::string> &value);
    bool SetServiceInfo(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);
    bool GetCouponList(const std::string &key, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value);
    bool SetCouponList(const std::string &key, const std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> &value);
    bool GetUbiList(const std::string &key, std::tuple<std::string, std::string> &value);
    bool SetUbiList(const std::string &key, const std::tuple<std::string, std::string> &value);
    bool GetDexList(const std::string &key, std::tuple<std::string, std::string, std::string> &value);
    bool SetDexList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);
    bool GetBookList(const std::string &key, std::tuple<std::string, std::string, std::string> &value);
    bool SetBookList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);
    bool GetNPList(const std::string &key, std::tuple<std::string, std::string, std::string> &value);
    bool SetNPList(const std::string &key, const std::tuple<std::string, std::string, std::string> &value);
    bool HaveCoins(const uint256 &txid);
    uint256 GetBestBlock();
    bool SetBestBlock(const uint256 &hashBlock);
    bool BatchWrite(const std::map<uint256, CCoins> &mapCoins, const std::map<CScript, std::pair<int64_t,int> > &mapAddressInfo,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceInfo,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string, std::string, std::string, std::string> > &mapServiceCouponList,
                    const std::map<std::string, std::tuple<std::string, std::string> > &mapServiceUbiList,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceDexList,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceBookList,
                    const std::map<std::string, std::tuple<std::string, std::string, std::string> > &mapServiceNPList,
                    const uint256 &hashBlock);

    //Return a modifiable reference to a CCoins. Check HaveCoins first.
    CCoins &GetCoins(const uint256 &txid);

    //Push the modifications applied to this cache to its base.
    bool Flush();

    //Calculate the size of the cache (in number of transactions)
    unsigned int GetCacheSize();

    //Amount of bitcoins coming in to a transaction
    int64_t GetValueIn(const CTransaction& tx);

    //Check whether all prevouts of the transaction are present in the UTXO set represented by this view
    bool HaveInputs(const CTransaction& tx);

    //Return priority of tx at height nHeight
    double GetPriority(const CTransaction &tx, int nHeight);

    const CTxOut &GetOutputFor(const CTxIn& input);

private:
    std::map<uint256,CCoins>::iterator FetchCoins(const uint256 &txid);
};

#endif
