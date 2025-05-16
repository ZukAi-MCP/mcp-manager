// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2016 The Auroracoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"

#include "assert.h"
#include "core.h"
#include "protocol.h"
#include "util.h"
#include "scrypt.h"

#include <boost/assign/list_of.hpp>

using namespace boost::assign;

//
// Main network
//

unsigned int pnSeed[] =
{
    0xcb47d082
};

class CMainParams : public CChainParams {
public:
    CMainParams() {
        // The message start string is designed to be unlikely to occur in normal data.
        pchMessageStart[0] = 0xfb;
        pchMessageStart[1] = 0xc0;
        pchMessageStart[2] = 0xb6;
        pchMessageStart[3] = 0xdb;
        vAlertPubKey = ParseHex("04d1832d7d0c59634d67d3023379403014c2878d0c2372d175219063a48fa06e6d429e09f36d3196ec544c2cfdd12d6fe510a399595f75ebb6da238eb5f70f2072");
        nDefaultPort = 11337;
        nRPCPort = 14242;

        bnProofOfWorkLimit[ALGO_SHA256D] = CBigNum(~uint256(0) >> 20); // 1.00000000
        bnProofOfWorkLimit[ALGO_SCRYPT]  = CBigNum(~uint256(0) >> 20);
        bnProofOfWorkLimit[ALGO_GROESTL] = CBigNum(~uint256(0) >> 20); // 0.00195311
        bnProofOfWorkLimit[ALGO_SKEIN]   = CBigNum(~uint256(0) >> 20); // 0.00195311
        bnProofOfWorkLimit[ALGO_QUBIT]   = CBigNum(~uint256(0) >> 20); // 0.00097655

        // Build the genesis block.
        const char* pszTimestamp = "NY Times 18/Aug/2014 Bitcoin's Price Falls 12%, to Lowest Value Since May";
        CMutableTransaction txNew;
        txNew.vin.resize(1);
        txNew.vout.resize(1);
        txNew.vin[0].scriptSig = CScript() << 486604799 << CBigNum(4) << vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
        txNew.vout[0].nValue = 10000 * COIN;
        txNew.vout[0].scriptPubKey = CScript() << ParseHex("040184710fa689ad5023690c80f3a49c8f13f8d45b8c857fbcbc8bc4a8e4d3eb4b10f4d4604fa08dce601aaf0f470216fe1b51850b4acf21b179c45070ac7b03a9") << OP_CHECKSIG;
        genesis.vtx.push_back(txNew);
        genesis.hashPrevBlock = 0;
        genesis.hashMerkleRoot = genesis.BuildMerkleTree();
        genesis.nVersion = 1;
        genesis.nTime    = 1408974288;
        genesis.nBits	 = 0x1e0ffff0;
        //genesis.nBits    = Params().ProofOfWorkLimit(ALGO_SCRYPT).GetCompact();
        //genesis.nBits = 0x1e0fffff;
        genesis.nNonce   = 386703170;

        hashGenesisBlock = genesis.GetHash();

        assert(hashGenesisBlock == uint256("0x660f734cf6c6d16111bde201bbd2122873f2f2c078b969779b9d4c99732354fd"));
        assert(genesis.hashMerkleRoot == uint256("0xe9441ec39c399c76ea734ea31827e1895a82c5a1f9b2c6252b5dacada768ec8b"));
	
		vSeeds.push_back(CDNSSeedData("smileyco.in", "dnsseed.smileyco.in"));

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,25); // Smileycoin addresses start with S
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,5);
        base58Prefixes[SECRET_KEY]     = std::vector<unsigned char>(1,153); // 25 + 128
        base58Prefixes[SECRET_KEY_OLD] = std::vector<unsigned char>(1,151);
        base58Prefixes[EXT_PUBLIC_KEY] = list_of(0x1E)(0x56)(0x2D)(0x9A).convert_to_container<std::vector<unsigned char> >();
        base58Prefixes[EXT_SECRET_KEY] = list_of(0x1E)(0x56)(0x31)(0xBC).convert_to_container<std::vector<unsigned char> >();
        // Convert the pnSeeds array into usable address objects.
        for (unsigned int i = 0; i < ARRAYLEN(pnSeed); i++)
        {
            const int64_t nOneWeek = 7*24*60*60;
            struct in_addr ip;
            memcpy(&ip, &pnSeed[i], sizeof(ip));
            CAddress addr(CService(ip, GetDefaultPort()));
            addr.nTime = GetTime() - GetRand(nOneWeek) - nOneWeek;
            vFixedSeeds.push_back(addr);
        }
    }

    virtual const CBlock& GenesisBlock() const { return genesis; }
    virtual Network NetworkID() const { return CChainParams::MAIN; }

    virtual const vector<CAddress>& FixedSeeds() const {
        return vFixedSeeds;
    }
protected:
    CBlock genesis;
    vector<CAddress> vFixedSeeds;
};
static CMainParams mainParams;


// Testnet
class CTestNetParams : public CMainParams {
public:
    CTestNetParams() {
        // The message start string is designed to be unlikely to occur in normal data.
        pchMessageStart[0] = 0xfb;
        pchMessageStart[1] = 0xc0;
        pchMessageStart[2] = 0xb6;
        pchMessageStart[3] = 0xdd;  // the "d" seperates test net from main net

        nDefaultPort = 12337;
        nRPCPort = 14243;
        strDataDir = "testnet";

        bnProofOfWorkLimit[ALGO_SHA256D] = CBigNum(~uint256(0) >> 20); // 1.00000000
        bnProofOfWorkLimit[ALGO_SCRYPT]  = CBigNum(~uint256(0) >> 20);
        bnProofOfWorkLimit[ALGO_GROESTL] = CBigNum(~uint256(0) >> 20); // 0.00195311
        bnProofOfWorkLimit[ALGO_SKEIN]   = CBigNum(~uint256(0) >> 20); // 0.00195311
        bnProofOfWorkLimit[ALGO_QUBIT]   = CBigNum(~uint256(0) >> 20); // 0.00097655

        // Modify the testnet genesis block so the timestamp is valid for a later start.
        genesis.nTime = 1448114586;
        genesis.nNonce = 1979089;
        hashGenesisBlock = genesis.GetHash();
        assert(hashGenesisBlock == uint256("0x54810bfb46c7b0d7bbe184faa10d2352810b29d1cdfa5169ce3aed387d80b921"));

        // If genesis block hash does not match, then generate new genesis hash.
        if (hashGenesisBlock != uint256("0x54810bfb46c7b0d7bbe184faa10d2352810b29d1cdfa5169ce3aed387d80b921"))
        {
            printf("Searching for testnet genesis block...\n");
            // This will figure out a valid hash and Nonce if you're creating a different genesis block:
            //uint256 hashTarget = CBigNum().SetCompact(block.nBits).getuint256();
            uint256 hashTarget = CBigNum().SetCompact(genesis.nBits).getuint256();
            uint256 thash;
            uint256 bestfound;

            //static char scratchpad[SCRYPT_SCRATCHPAD_SIZE];
            scrypt_1024_1_1_256(BEGIN(genesis.nVersion), BEGIN(bestfound));

            while(true)
                {
                scrypt_1024_1_1_256(BEGIN(genesis.nVersion), BEGIN(thash));
                //thash = scrypt_blockhash(BEGIN(block.nVersion));
                if (thash <= hashTarget)
                    break;
                //if ((genesis.nNonce & 0xFFF) == 0)
                if (thash <= bestfound)
                    {
                    bestfound = thash;
                    printf("nonce %08X: hash = %s (target = %s)\n", genesis.nNonce, thash.ToString().c_str(), hashTarget.ToString().c_str());
                    }
                ++genesis.nNonce;
                if (genesis.nNonce == 0)
                    {
                    printf("NONCE WRAPPED, incrementing time\n");
                    ++genesis.nTime;
                    }
                }
           printf("block.nTime = %u \n", genesis.nTime);
           printf("block.nNonce = %u \n", genesis.nNonce);
           printf("block.GetHash = %s\n", genesis.GetHash().ToString().c_str());
           }

        vFixedSeeds.clear();
        vSeeds.clear();
        vSeeds.push_back(CDNSSeedData("localtests", "localhost"));
        //vSeeds.push_back(CDNSSeedData("testnet-united-states-east", "testnet1.auroraseed.com"));
        //vSeeds.push_back(CDNSSeedData("testnet-united-states-west", "testnet2.criptoe.com"));

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,58); // Smileycoin addresses start with S
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,12);
        base58Prefixes[SECRET_KEY]     = std::vector<unsigned char>(1,112); // 25 + 128
        base58Prefixes[SECRET_KEY_OLD] = std::vector<unsigned char>(1,148);
        base58Prefixes[EXT_PUBLIC_KEY] = list_of(0x1E)(0x56)(0x2D)(0x9A).convert_to_container<std::vector<unsigned char> >();
        base58Prefixes[EXT_SECRET_KEY] = list_of(0x1E)(0x56)(0x31)(0xBC).convert_to_container<std::vector<unsigned char> >();
    }
    virtual Network NetworkID() const { return CChainParams::TESTNET; }
};
static CTestNetParams testNetParams;


// Regression test
class CRegTestParams : public CTestNetParams {
public:
    CRegTestParams() {
        pchMessageStart[0] = 0xfa;
        pchMessageStart[1] = 0xbf;
        pchMessageStart[2] = 0xb5;
        pchMessageStart[3] = 0xda;
        //nSubsidyHalvingInterval = 150;
        // bnProofOfWorkLimit = CBigNum();
        genesis.nTime = 1296688602;
        genesis.nBits = 0x207fffff;
        genesis.nNonce = 0;
        hashGenesisBlock = genesis.GetHash();
        nDefaultPort = 19444;
        strDataDir = "regtest";
        //assert(hashGenesisBlock == uint256("0x0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206"));

        vSeeds.clear();  // Regtest mode doesn't have any DNS seeds.
    }

    virtual bool RequireRPCPassword() const { return false; }
    virtual Network NetworkID() const { return CChainParams::REGTEST; }
};
static CRegTestParams regTestParams;

static CChainParams *pCurrentParams = &mainParams;

const CChainParams &Params() {
    return *pCurrentParams;
}

void SelectParams(CChainParams::Network network) {
    switch (network) {
        case CChainParams::MAIN:
            pCurrentParams = &mainParams;
            break;
        case CChainParams::TESTNET:
            pCurrentParams = &testNetParams;
            break;
        case CChainParams::REGTEST:
            pCurrentParams = &regTestParams;
            break;
        default:
            assert(false && "Unimplemented network");
            return;
    }
}

bool SelectParamsFromCommandLine() {
    bool fRegTest = GetBoolArg("-regtest", false);
    bool fTestNet = GetBoolArg("-testnet", false);

    if (fTestNet && fRegTest) {
        return false;
    }

    if (fRegTest) {
        SelectParams(CChainParams::REGTEST);
    } else if (fTestNet) {
        SelectParams(CChainParams::TESTNET);
    } else {
        SelectParams(CChainParams::MAIN);
    }
    return true;
}
