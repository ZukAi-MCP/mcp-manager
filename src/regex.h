#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <regex>
#include <stdint.h>
//#include <filesystem>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include "util.h"

using namespace std;
using namespace boost;
namespace fs = boost::filesystem;

bool isRegex(const string& txData) {
    // If string begins with "ACCEPT offer "(in hex) and if string is 90 characters long we check the txid
    if (txData.rfind("414343455054206f6666657220", 0) == 0 && txData.length() == 90) {
        // Fetch the txid and the 64 characters which come after "ACCEPT offer "
        std::string txid = txData.substr(26, 64);
        // Use gettransaction with the txid - if not available then return false
        uint256 hash = ParseHashV(txid, "parameter 1");
        CTransaction tx;
        uint256 hashBlock = 0;
        if (!GetTransaction(hash, tx, hashBlock, true))
        {
            return false;
        }
    }

    std::vector<std::string> lines;
    string filename = "./smileyCoin/src/regex.txt"; // works only if smileyCoin is in working directory
    ifstream infile(filename.c_str());
    if (!infile.is_open())
    {
        return false;
    }

    string line;
    while (getline(infile, line))
    {
        istringstream iss(line);
        ostringstream oss;
        lines.push_back(line);
    }

    for (int i = 0; i < lines.size(); i++) {
        std::string strengur = lines[i];
        std::regex expr(strengur);

        if (std::regex_match(txData, expr)) {
            LogPrintStr(" 8 i regex: " + txData);

            return true;
        }
    }
    // send the data encrypted
    return false;

    infile.close();

}