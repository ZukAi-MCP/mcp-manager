// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


#ifndef BITCOIN_RICHLISTDB_H
#define BITCOIN_RICHLISTDB_H

#include "txdb.h"
#include "init.h" 
#include "core.h"

class CRichList;
extern CRichList RichList;

struct RichOrderCompare
{
    bool operator()(const std::pair< CScript, std::pair<int64_t, int> >& lhs, const std::pair< CScript, std::pair<int64_t, int> >& rhs)
    {
        return lhs.second.second < rhs.second.second;
    }
};

typedef std::pair<int64_t, int> CAddressInfo;
typedef std::map< CScript, CAddressInfo> mapScriptPubKeys;

class CRichList
{
private:
	mapScriptPubKeys maddresses;
	bool fForked; 

	CScript ScriptPubKey(const mapScriptPubKeys::iterator &it) const { return it -> first; }
	int Height(const mapScriptPubKeys::iterator &it) const { return it -> second.second; }
	int64_t Balance(const mapScriptPubKeys::iterator &it) const { return it -> second.first; }
	int Height(const mapScriptPubKeys::const_iterator &it) const { return it -> second.second; }
	int64_t Balance(const mapScriptPubKeys::const_iterator &it) const { return it -> second.first; }

public:

	friend bool CCoinsViewDB::GetRichAddresses(CRichList &richlist);
	bool GetRichAddresses(std::multiset< std::pair< CScript, std::pair<int64_t, int> >, RichOrderCompare > &retset) const;

	bool SetForked(const bool &fFork);
	bool IsForked(){return fForked;}
// henda út?
	
	bool UpdateAddressInfo(const std::map<CScript, std::pair<int64_t, int> > &map);
	bool UpdateRichAddressHeights();


	bool NextRichScriptPubKey(CScript &scriptpubkey);

	CScript NextEIASScriptPubKey(const int &nHeight);

};




#endif
