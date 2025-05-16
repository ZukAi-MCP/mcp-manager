#ifndef BITCOIN_JEEQ_H
#define BITCOIN_JEEQ_H

class CBitcoinAddress;

namespace Jeeq
{
std::vector<uint8_t> EncryptMessage(const CPubKey pubkey, const std::string msg);
std::string DecryptMessage(const CKey privkey, const std::vector<uint8_t> enc);
CPubKey SearchForPubKey(CBitcoinAddress addr);
}

#endif

