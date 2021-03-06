/**
* Copyright (c) 2018 Zilliqa 
* This source code is being disclosed to you solely for the purpose of your participation in 
* testing Zilliqa. You may view, compile and run the code for that purpose and pursuant to 
* the protocols and algorithms that are programmed into, and intended by, the code. You may 
* not do anything else with the code without express permission from Zilliqa Research Pte. Ltd., 
* including modifying or publishing the code (or any part of it), and developing or forming 
* another public or private blockchain network. This source code is provided ‘as is’ and no 
* warranties are given as to title or non-infringement, merchantability or fitness for purpose 
* and, to the extent permitted by law, all liability for your use of the code is disclaimed. 
* Some programs in this code are governed by the GNU General Public License v3.0 (available at 
* https://www.gnu.org/licenses/gpl-3.0.en.html) (‘GPLv3’). The programs that are governed by 
* GPLv3.0 are those programs that are located in the folders src/depends and tests/depends 
* and which include a reference to GPLv3 in their program files.
**/

#include <algorithm>
#include <chrono>
#include <thread>

#include "DirectoryService.h"
#include "common/Constants.h"
#include "common/Messages.h"
#include "common/Serializable.h"
#include "depends/common/RLP.h"
#include "depends/libDatabase/MemoryDB.h"
#include "depends/libTrie/TrieDB.h"
#include "depends/libTrie/TrieHash.h"
#include "libCrypto/Sha2.h"
#include "libMediator/Mediator.h"
#include "libNetwork/P2PComm.h"
#include "libNetwork/Whitelist.h"
#include "libUtils/DataConversion.h"
#include "libUtils/DetachedFunction.h"
#include "libUtils/Logger.h"
#include "libUtils/SanityChecks.h"

using namespace std;
using namespace boost::multiprecision;

bool DirectoryService::VerifyPoWSubmission(
    const vector<unsigned char>& message, const Peer& from, PubKey& key,
    unsigned int curr_offset, uint32_t& portNo, uint64_t& nonce,
    array<unsigned char, 32>& rand1, array<unsigned char, 32>& rand2,
    uint8_t& difficultyLevel, uint64_t& block_num, string& winning_hash)
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "DirectoryService::VerifyPoWSubmission not expected to be "
                    "called from LookUp node.");
        return true;
    }

    // 8-byte nonce
    nonce = Serializable::GetNumber<uint64_t>(message, curr_offset,
                                              sizeof(uint64_t));
    curr_offset += sizeof(uint64_t);

    // 32-byte resulting hash
    winning_hash = DataConversion::Uint8VecToHexStr(message, curr_offset,
                                                    BLOCK_HASH_SIZE);
    curr_offset += BLOCK_HASH_SIZE;

    // 32-byte mixhash
    string winning_mixhash = DataConversion::Uint8VecToHexStr(
        message, curr_offset, BLOCK_HASH_SIZE);

    curr_offset += BLOCK_HASH_SIZE;

    //64-byte signature
    Signature sign(message, curr_offset);

    if (!Schnorr::GetInstance().Verify(message, 0, curr_offset, sign, key))
    {
        LOG_GENERAL(WARNING, "PoW submission signature wrong");
        return false;
    }

    curr_offset += SIGNATURE_CHALLENGE_SIZE + SIGNATURE_RESPONSE_SIZE;
    // Log all values
    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Winner Public_key             = 0x"
                  << DataConversion::SerializableToHexStr(key));
    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "Winner Peer ip addr           = " << from.GetPrintableIPAddress()
                                                 << ":" << portNo);

    // Define the PoW parameters
    rand1 = m_mediator.m_dsBlockRand;
    rand2 = m_mediator.m_txBlockRand;

    // Verify nonce
    block_num
        = m_mediator.m_dsBlockChain.GetLastBlock().GetHeader().GetBlockNum()
        + 1;
    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "dsblock_num            = " << block_num);

    uint8_t expectedDSDiff = DS_POW_DIFFICULTY;
    uint8_t expectedDiff = POW_DIFFICULTY;

    // Non-gensis block
    if (block_num > 1)
    {
        expectedDSDiff = m_mediator.m_dsBlockChain.GetLastBlock()
                             .GetHeader()
                             .GetDSDifficulty();
        expectedDiff = m_mediator.m_dsBlockChain.GetLastBlock()
                           .GetHeader()
                           .GetDifficulty();
    }

    if (difficultyLevel != expectedDSDiff && difficultyLevel != expectedDiff)
    {
        LOG_GENERAL(WARNING,
                    "Difficulty level is invalid. difficultyLevel: "
                        << to_string(difficultyLevel)
                        << " Expected: " << to_string(expectedDSDiff) << " or "
                        << to_string(expectedDiff));

        // TODO: penalise sender in reputation manager
        return false;
    }

    m_timespec = r_timer_start();
    bool result = POW::GetInstance().PoWVerify(
        block_num, difficultyLevel, rand1, rand2, from.m_ipAddress, key, false,
        nonce, winning_hash, winning_mixhash);
    LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
              "[POWSTAT] pow verify (microsec): " << r_timer_end(m_timespec));

    return result;
}

bool DirectoryService::ParseMessageAndVerifyPOW(
    const vector<unsigned char>& message, unsigned int offset, const Peer& from)
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "DirectoryService::ParseMessageAndVerifyPOW not expected "
                    "to be called from LookUp node.");
        return true;
    }

    unsigned int curr_offset = offset;
    // 8-bytes block number
    uint64_t DSBlockNum = Serializable::GetNumber<uint64_t>(
        message, curr_offset, sizeof(uint64_t));
    curr_offset += sizeof(uint64_t);

    // 1-byte difficultyLevel
    uint8_t difficultyLevel = Serializable::GetNumber<uint8_t>(
        message, curr_offset, sizeof(uint8_t));
    curr_offset += sizeof(uint8_t);

    // Check block number
    if (!CheckWhetherDSBlockIsFresh(DSBlockNum))
    {
        return false;
    }

    // 4-byte listening port
    uint32_t portNo = Serializable::GetNumber<uint32_t>(message, curr_offset,
                                                        sizeof(uint32_t));
    curr_offset += sizeof(uint32_t);

    uint128_t ipAddr = from.m_ipAddress;
    Peer peer(ipAddr, portNo);

    // 33-byte public key
    // PubKey key(message, curr_offset);
    PubKey key;
    if (key.Deserialize(message, curr_offset) != 0)
    {
        LOG_GENERAL(WARNING, "We failed to deserialize PubKey.");
        return false;
    }
    curr_offset += PUB_KEY_SIZE;

    if (TEST_NET_MODE
        && not Whitelist::GetInstance().IsNodeInDSWhiteList(peer, key))
    {
        LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Submitted PoW but node is not in DS whitelist. Hence, "
                  "not accepted!");
    }

    // Todo: Reject PoW submissions from existing members of DS committee

    if (!CheckState(VERIFYPOW))
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Too late - current state is "
                      << m_state
                      << ". Don't verify cause I have other work to do. "
                         "Assume true as it has no impact.");
        return true;
    }

    if (!Whitelist::GetInstance().IsValidIP(peer.m_ipAddress))
    {
        LOG_GENERAL(WARNING,
                    "IP belong to private ip subnet or is a broadcast address");
        return false;
    }

    uint64_t nonce;
    array<unsigned char, 32> rand1;
    array<unsigned char, 32> rand2;
    uint64_t block_num;

    if (CheckPoWSubmissionExceedsLimitsForNode(key))
    {
        LOG_GENERAL(WARNING, peer << "  has exceeded max pow submission ");
        return false;
    }

    string winning_hash;
    bool result = VerifyPoWSubmission(message, from, key, curr_offset, portNo,
                                      nonce, rand1, rand2, difficultyLevel,
                                      block_num, winning_hash);
    if (result)
    {
        // Do another check on the state before accessing m_allPoWs
        // Accept slightly late entries as we need to multicast the DSBLOCK to everyone
        // if ((m_state != POW_SUBMISSION) && (m_state != DSBLOCK_CONSENSUS_PREP))
        if (!CheckState(VERIFYPOW))
        {
            LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "Too late - current state is " << m_state);
        }
        else
        {
            LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "POW verification passed");
            lock(m_mutexAllPOW, m_mutexAllPoWConns);
            lock_guard<mutex> g(m_mutexAllPOW, adopt_lock);
            lock_guard<mutex> g2(m_mutexAllPoWConns, adopt_lock);

            std::array<unsigned char, 32> winningHashArr
                = DataConversion::HexStrToStdArray(winning_hash);

            m_allPoWConns.emplace(key, peer);
            m_allPoWs[key] = winningHashArr;

            uint8_t expectedDSDiff = DS_POW_DIFFICULTY;
            if (block_num > 1)
            {
                expectedDSDiff = m_mediator.m_dsBlockChain.GetLastBlock()
                                     .GetHeader()
                                     .GetDSDifficulty();
            }

            if (difficultyLevel == expectedDSDiff)
            {
                AddDSPoWs(key, winningHashArr);
            }

            UpdatePoWSubmissionCounterforNode(key);
        }
    }
    else
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Invalid PoW submission"
                      << "\n"
                      << "blockNum: " << block_num << " Difficulty: "
                      << to_string(difficultyLevel) << " nonce: " << nonce
                      << " ip: " << peer.GetPrintableIPAddress() << ":"
                      << portNo << "\n"
                      << "rand1: " << DataConversion::charArrToHexStr(rand1)
                      << " rand2: " << DataConversion::charArrToHexStr(rand2));
    }
    return result;
}

bool DirectoryService::CheckPoWSubmissionExceedsLimitsForNode(const PubKey& key)
{
    lock_guard<mutex> g(m_mutexAllPoWCounter);
    if (m_AllPoWCounter.find(key) == m_AllPoWCounter.end())
    {
        return false;
    }
    else if (m_AllPoWCounter[key] < POW_SUBMISSION_LIMIT)
    {
        return false;
    }
    return true;
}

void DirectoryService::UpdatePoWSubmissionCounterforNode(const PubKey& key)
{
    lock_guard<mutex> g(m_mutexAllPoWCounter);

    if (m_AllPoWCounter.find(key) == m_AllPoWCounter.end())
    {
        m_AllPoWCounter.emplace(key, 1);
    }
    else
    {
        m_AllPoWCounter[key] = m_AllPoWCounter[key] + 1;
    }
}

void DirectoryService::ResetPoWSubmissionCounter()
{
    lock_guard<mutex> g(m_mutexAllPoWCounter);
    m_AllPoWCounter.clear();
}

bool DirectoryService::ProcessPoWSubmission(
    const vector<unsigned char>& message, unsigned int offset, const Peer& from)
{
    if (LOOKUP_NODE_MODE)
    {
        LOG_GENERAL(WARNING,
                    "DirectoryService::ProcessPoWSubmission not expected to be "
                    "called from LookUp node.");
        return true;
    }

    // Message = [8-byte block number] [4-byte listening port] [33-byte public key] [8-byte nonce] [32-byte resulting hash]
    //[32-byte mixhash] [64-byte Sign]
    LOG_MARKER();

    if (m_state == FINALBLOCK_CONSENSUS)
    {
        std::unique_lock<std::mutex> cv_lk(m_MutexCVPOWSubmission);

        if (cv_POWSubmission.wait_for(
                cv_lk, std::chrono::seconds(POW_SUBMISSION_TIMEOUT))
            == std::cv_status::timeout)
        {
            LOG_EPOCH(WARNING, to_string(m_mediator.m_currentEpochNum).c_str(),
                      "Time out while waiting for state transition ");
        }

        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "State transition is completed. (check for timeout)");
    }

    if (!CheckState(PROCESS_POWSUBMISSION))
    {
        LOG_EPOCH(INFO, to_string(m_mediator.m_currentEpochNum).c_str(),
                  "Not at POW_SUBMISSION. Current state is " << m_state);
        return false;
    }

    if (IsMessageSizeInappropriate(
            message.size(), offset,
            sizeof(uint64_t) + sizeof(uint32_t) + PUB_KEY_SIZE
                + sizeof(uint64_t) + BLOCK_HASH_SIZE + BLOCK_HASH_SIZE
                + SIGNATURE_CHALLENGE_SIZE + SIGNATURE_RESPONSE_SIZE))
    {
        LOG_GENERAL(WARNING, "Pow message size Inappropriate ");
        return false;
    }

    bool result = ParseMessageAndVerifyPOW(message, offset, from);
    return result;
}

void DirectoryService::AddDSPoWs(PubKey Pubk,
                                 std::array<unsigned char, 32> DSPOWSoln)
{
    lock_guard<mutex> g(m_mutexAllDSPOWs);
    m_allDSPoWs[Pubk] = DSPOWSoln;
}
std::map<PubKey, std::array<unsigned char, 32>> DirectoryService::GetAllDSPoWs()
{
    lock_guard<mutex> g(m_mutexAllDSPOWs);
    return m_allDSPoWs;
}

void DirectoryService::clearDSPoWSolns()
{
    lock_guard<mutex> g(m_mutexAllDSPOWs);
    m_allDSPoWs.clear();
}

std::array<unsigned char, 32> DirectoryService::GetDSPoWSoln(PubKey Pubk)
{
    lock_guard<mutex> g(m_mutexAllDSPOWs);
    if (m_allDSPoWs.find(Pubk) != m_allDSPoWs.end())
    {
        return m_allDSPoWs[Pubk];
    }
    else
    {
        LOG_GENERAL(WARNING, "No such element in m_allDSPoWs");
        return array<unsigned char, 32>();
    }
}

bool DirectoryService::IsNodeSubmittedDSPoWSoln(PubKey Pubk)
{
    lock_guard<mutex> g(m_mutexAllDSPOWs);
    if (m_allDSPoWs.find(Pubk) != m_allDSPoWs.end())
    {
        return true;
    }
    return false;
}

uint32_t DirectoryService::GetNumberOfDSPoWSolns()
{
    lock_guard<mutex> g(m_mutexAllDSPOWs);
    return m_allDSPoWs.size();
}