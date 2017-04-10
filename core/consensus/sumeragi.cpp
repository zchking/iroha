/*
Copyright Soramitsu Co., Ltd. 2016 All Rights Reserved.
http://soramitsu.co.jp
Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at
         http://www.apache.org/licenses/LICENSE-2.0
Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "sumeragi.hpp"
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <queue>
#include <string>
#include <thread>

#include <thread_pool.hpp>

#include <crypto/hash.hpp>
#include <crypto/signature.hpp>
#include <util/logger.hpp>

#include <infra/config/peer_service_with_json.hpp>
#include <service/peer_service.hpp>

#include "connection/connection.hpp"

#include <infra/config/iroha_config_with_json.hpp>
#include <infra/config/peer_service_with_json.hpp>
#include <service/executor.hpp>

#include <crypto/base64.hpp>
/**
 * |ーーー|　|ーーー|　|ーーー|　|ーーー|
 * |　ス　|ー|　メ　|ー|　ラ　|ー|　ギ　|
 * |ーーー|　|ーーー|　|ーーー|　|ーーー|
 *
 * A chain-based byzantine fault tolerant consensus algorithm, based in large
 * part on BChain:
 *
 * Duan, S., Meling, H., Peisert, S., & Zhang, H. (2014). Bchain: Byzantine
 * replication with high throughput and embedded reconfiguration. In
 * International Conference on Principles of Distributed Systems (pp. 91-106).
 * Springer.
 */
namespace sumeragi {


using iroha::ConsensusEvent;
using iroha::Signature;
using iroha::Transaction;

std::map<std::string, std::string> txCache;

static ThreadPool pool(ThreadPoolOptions{
    .threads_count =
        config::IrohaConfigManager::getInstance().getConcurrency(0),
    .worker_queue_size =
        config::IrohaConfigManager::getInstance().getPoolWorkerQueueSize(1024),
});

namespace detail {

std::string hash(const std::unique_ptr<Transaction>& tx) {
  // ToDo We should make tx.to_string()
  return hash::sha3_256_hex("");
};

std::unique_ptr<ConsensusEvent> addSignature(
    std::unique_ptr<ConsensusEvent> event, const std::string& publicKey,
    const std::string& signature) {
  // Copy
  flatbuffers::FlatBufferBuilder fbb;
  std::unique_ptr<std::vector<flatbuffers::Offset<Signature>>> signatures(
      new std::vector<flatbuffers::Offset<Signature>>());
  std::unique_ptr<std::vector<flatbuffers::Offset<Transaction>>> transactions(
      new std::vector<flatbuffers::Offset<Transaction>>());

  for (auto&& sig : *event->peerSignatures()) {
    signatures->emplace_back(reinterpret_cast<flatbuffers::uoffset_t>(sig));
  }
  for (auto&& tx : *event->transactions()) {
    transactions->emplace_back(tx);
  }

  // Add signature
  std::vector<uint8_t>* encodedSignaturePtr = new std::vector<uint8_t>();
  auto encodedSignature = base64::decode(signature);
  for (auto&& c : encodedSignature) {
    encodedSignaturePtr->push_back(c);
  }
  signatures->emplace_back(iroha::CreateSignatureDirect(fbb, publicKey.c_str(),
                                                        encodedSignaturePtr));

  // Create
  auto event_buf = iroha::CreateConsensusEventDirect(fbb, signatures.get(),
                                                     transactions.get());
  fbb.Finish(event_buf);

  // ToDo I think std::unique_ptr<const T> is not popular. Is it?
  return std::unique_ptr<ConsensusEvent>(const_cast<ConsensusEvent*>(
      flatbuffers::GetRoot<ConsensusEvent>(fbb.GetBufferPointer())));
}

bool eventSignatureIsEmpty(const std::unique_ptr<ConsensusEvent>& event) {
  return event->peerSignatures()->size() == 0;
}

void printJudge(int numValidSignatures, int numValidationPeer, int faulty) {
  std::stringstream resLine[5];
  for (int i = 0; i < numValidationPeer; i++) {
    if (i < numValidSignatures) {
      resLine[0] << "\033[1m\033[92m+-ー-+\033[0m";
      resLine[1] << "\033[1m\033[92m| 　 |\033[0m";
      resLine[2] << "\033[1m\033[92m|-承-|\033[0m";
      resLine[3] << "\033[1m\033[92m| 　 |\033[0m";
      resLine[4] << "\033[1m\033[92m+-＝-+\033[0m";
    } else {
      resLine[0] << "\033[91m+-ー-+\033[0m";
      resLine[1] << "\033[91m| 　 |\033[0m";
      resLine[2] << "\033[91m| 否 |\033[0m";
      resLine[3] << "\033[91m| 　 |\033[0m";
      resLine[4] << "\033[91m+-＝-+\033[0m";
    }
  }
  for (int i = 0; i < 5; ++i) {
    logger::explore("sumeragi") << resLine[i].str();
  }
  std::string line;
  for (int i = 0; i < numValidationPeer; i++) line += "==＝==";
  logger::explore("sumeragi") << line;

  logger::explore("sumeragi")
      << "numValidSignatures:" << numValidSignatures << " faulty:" << faulty;
}

void printAgree() {
  logger::explore("sumeragi") << "\033[1m\033[92m+==ーー==+\033[0m";
  logger::explore("sumeragi") << "\033[1m\033[92m|+-ーー-+|\033[0m";
  logger::explore("sumeragi") << "\033[1m\033[92m|| 承認 ||\033[0m";
  logger::explore("sumeragi") << "\033[1m\033[92m|+-ーー-+|\033[0m";
  logger::explore("sumeragi") << "\033[1m\033[92m+==ーー==+\033[0m";
}

void printReject() {
  logger::explore("sumeragi") << "\033[91m+==ーー==+\033[0m";
  logger::explore("sumeragi") << "\033[91m|+-ーー-+|\033[0m";
  logger::explore("sumeragi") << "\033[91m|| 否認 ||\033[0m";
  logger::explore("sumeragi") << "\033[91m|+-ーー-+|\033[0m";
  logger::explore("sumeragi") << "\033[91m+==ーー==+\033[0m";
}
}  // namespace detail

struct Context {
  bool isSumeragi;          // am I the leader or am I not?
  std::uint64_t maxFaulty;  // f
  std::uint64_t proxyTailNdx;
  std::int32_t panicCount;
  std::int64_t commitedCount = 0;
  std::uint64_t numValidatingPeers;
  std::string myPublicKey;
  std::deque<std::unique_ptr<peer::Node>> validatingPeers;

  Context() { update(); }

  Context(std::vector<std::unique_ptr<peer::Node>>&& peers) {
    for (auto&& p : peers) {
      validatingPeers.push_back(std::move(p));
    }
  }

  void update() {
    /*
    auto peers = config::PeerServiceConfig::getInstance().getPeerList();
    for (auto&& p : peers ) {
        validatingPeers.push_back( std::move(p) );
    }

    this->numValidatingPeers = this->validatingPeers.size();
    // maxFaulty = Default to approx. 1/3 of the network.
    this->maxFaulty = config::IrohaConfigManager::getInstance()
            .getMaxFaultyPeers(this->numValidatingPeers / 3);
    this->proxyTailNdx = this->maxFaulty * 2 + 1;

    if (this->validatingPeers.empty()) {
        logger::error("sumeragi") << "could not find any validating peers.";
        exit(EXIT_FAILURE);
    }

    if (this->proxyTailNdx >= this->validatingPeers.size()) {
        this->proxyTailNdx = this->validatingPeers.size() - 1;
    }

    this->panicCount = 0;
    this->myPublicKey =
    config::PeerServiceConfig::getInstance().getMyPublicKey();

    this->isSumeragi = this->validatingPeers.at(0)->getPublicKey() ==
    this->myPublicKey;
    */
  }
};

std::unique_ptr<Context> context = nullptr;

void initializeSumeragi() {
  logger::explore("sumeragi") << "\033[95m+==ーーーーーーーーー==+\033[0m";
  logger::explore("sumeragi") << "\033[95m|+-ーーーーーーーーー-+|\033[0m";
  logger::explore("sumeragi") << "\033[95m|| 　　　　　　　　　 ||\033[0m";
  logger::explore("sumeragi") << "\033[95m|| いろは合意形成機構 ||\033[0m";
  logger::explore("sumeragi")
      << "\033[95m|| 　　　\033[1mすめらぎ\033[0m\033[95m　　 ||\033[0m";
  logger::explore("sumeragi") << "\033[95m|| 　　　　　　　　　 ||\033[0m";
  logger::explore("sumeragi") << "\033[95m|+-ーーーーーーーーー-+|\033[0m";
  logger::explore("sumeragi") << "\033[95m+==ーーーーーーーーー==+\033[0m";
  logger::explore("sumeragi") << "- 起動/setup";
  logger::explore("sumeragi") << "- 初期設定/initialize";
  // merkle_transaction_repository::initLeaf();

  logger::info("sumeragi") << "My key is "
                           << config::PeerServiceConfig::getInstance()
                                  .getMyIpWithDefault("Invalid!!");
  logger::info("sumeragi") << "Sumeragi setted";
  logger::info("sumeragi") << "set number of validatingPeer";

  context = std::make_unique<Context>();

  connection::iroha::SumeragiImpl::Torii::receive(
      [](const std::string& from, Transaction&& transaction) {
        logger::info("sumeragi") << "receive!";
        // ToDo add build signature function.

        // send processTransaction(event) as a task to processing pool
        // this returns std::future<void> object
        // (std::future).get() method locks processing until result of
        // processTransaction will be available but processTransaction returns
        // void, so we don't have to call it and wait
        // std::function<void()> &&task = std::bind(processTransaction, event);
        // pool.process(std::move(task));
      });

  connection::iroha::SumeragiImpl::Verify::receive(
      [](const std::string& from, ConsensusEvent&& event) {
        logger::info("sumeragi") << "receive!";  // ToDo rewrite
        logger::info("sumeragi") << "received message! sig:["
                                 << 1 /*event.eventsignatures_size()*/ << "]";

        // mklogger::info("sumeragi") << "received message! status:[" <<
        // event.status() << "]";  if(event.status() == "commited") {
        // if(txCache.find(detail::hash(event.transaction())) == txCache.end())
        // {  executor::execute(event.transaction());
        // txCache[detail::hash(event.transaction())] = "commited";
        //}
        //}else{
        // send processTransaction(event) as a task to processing pool
        // this returns std::future<void> object
        // (std::future).get() method locks processing until result of
        // processTransaction will be available but processTransaction returns
        // void, so we don't have to call it and wait
        //    std::function<void()> &&task = std::bind(processTransaction,
        //    event); pool.process(std::move(task));
        //}
      });

  logger::info("sumeragi") << "initialize numValidatingPeers :"
                           << context->numValidatingPeers;
  logger::info("sumeragi") << "initialize maxFaulty :" << context->maxFaulty;
  logger::info("sumeragi") << "initialize proxyTailNdx :"
                           << context->proxyTailNdx;

  logger::info("sumeragi") << "initialize panicCount :" << context->panicCount;
  logger::info("sumeragi") << "initialize myPublicKey :"
                           << context->myPublicKey;

  // TODO: move the peer service and ordering code to another place
  // determineConsensusOrder(); // side effect is to modify validatingPeers
  logger::info("sumeragi") << "initialize is sumeragi :"
                           << static_cast<int>(context->isSumeragi);
  logger::info("sumeragi") << "initialize.....  complete!";
}


std::uint64_t getNextOrder() {
  return 0l;
  // ToDo
  // return merkle_transaction_repository::getLastLeafOrder() + 1;
}


void processTransaction(std::unique_ptr<ConsensusEvent> event) {
  logger::info("sumeragi") << "processTransaction";
  // if (!transaction_validator::isValid(event->getTx())) {
  //    return; //TODO-futurework: give bad trust rating to nodes that sent an
  //    invalid event
  //}
  logger::info("sumeragi") << "valid";
  logger::info("sumeragi") << "Add my signature...";

  logger::info("sumeragi") << "hash:"
                           << "";  // detail::hash(event.transaction());
  /*
  logger::info("sumeragi")    <<  "pub: "  <<
  config::PeerServiceConfig::getInstance().getMyPublicKey();
  logger::info("sumeragi")    <<  "priv:"  <<
  config::PeerServiceConfig::getInstance().getMyPrivateKey();
  logger::info("sumeragi")    <<  "sig: "  <<  signature::sign(
          // ToDo We should add it.
          //detail::hash(event.transaction()),
          "",
          config::PeerServiceConfig::getInstance().getMyPublicKey(),
          config::PeerServiceConfig::getInstance().getMyPrivateKey()
  );
  //detail::printIsSumeragi(context->isSumeragi);
  // Really need? blow "if statement" will be false anytime.
  event = detail::addSignature(std::move(event),
                               config::PeerServiceConfig::getInstance().getMyPublicKey(),
                               signature::sign(
                                       // ToDo We should add it.
                                       // detail::hash(event.transaction()))
                                       "",
                                       config::PeerServiceConfig::getInstance().getMyPublicKey(),
                                       config::PeerServiceConfig::getInstance().getMyPrivateKey()
                               )
  );
  */
  if (detail::eventSignatureIsEmpty(event) && context->isSumeragi) {
    logger::info("sumeragi") << "signatures.empty() isSumragi";
    // Determine the order for processing this event
    // event.set_order(getNextOrder());//TODO getNexOrder is always return 0l;
    // logger::info("sumeragi") << "new  order:" << event.order();
  } else if (!detail::eventSignatureIsEmpty(event)) {
    // Check if we have at least 2f+1 signatures needed for Byzantine fault
    // tolerance ToDo re write
    // if (transaction_validator::countValidSignatures(event) >=
    // context->maxFaulty * 2 + 1) {
    if (true) {
      logger::info("sumeragi") << "Signature exists";

      logger::explore("sumeragi") << "0--------------------------0";
      logger::explore("sumeragi") << "+~~~~~~~~~~~~~~~~~~~~~~~~~~+";
      logger::explore("sumeragi") << "|Would you agree with this?|";
      logger::explore("sumeragi") << "+~~~~~~~~~~~~~~~~~~~~~~~~~~+";
      logger::explore("sumeragi") << "\033[93m0================================"
                                     "================================0\033[0m";
      // ToDo
      // logger::explore("sumeragi") <<  "\033[93m0\033[1m"  <<
      // detail::hash(event.transaction())  <<  "0\033[0m";
      logger::explore("sumeragi") << "\033[93m0================================"
                                     "================================0\033[0m";

      detail::printJudge(
          // ToDo Re write
          1  // transaction_validator::countValidSignatures(event)
          ,
          context->numValidatingPeers, context->maxFaulty * 2 + 1);

      detail::printAgree();
      // Check Merkle roots to see if match for new state
      // TODO: std::vector<std::string>>const merkleSignatures =
      // event.merkleRootSignatures;
      // Try applying transaction locally and compute the merkle root

      // Commit locally
      logger::explore("sumeragi") << "commit";

      context->commitedCount++;

      logger::explore("sumeragi") << "commit count:" << context->commitedCount;
      // ToDo
      // event.set_status("commited");
      // connection::iroha::Sumeragi::Verify::sendAll(std::move(event));

    } else {
      // This is a new event, so we should verify, sign, and broadcast it
      /*
       * detail::addSignature(
         event,
         config::PeerServiceConfig::getInstance().getMyPublicKey(),
         "",
         // ToDo
         config::PeerServiceConfig::getInstance().getMyPublicKey().
         config::PeerServiceConfig::getInstance().getMyPrivateKey()).c_str()
      );

      logger::info("sumeragi")        <<  "tail public key is "   <<
      context->validatingPeers.at(context->proxyTailNdx)->getPublicKey();
      logger::info("sumeragi")        <<  "tail is "              <<
      context->proxyTailNdx; logger::info("sumeragi")        <<  "my public key
      is "     <<  config::PeerServiceConfig::getInstance().getMyPublicKey();

      if (context->validatingPeers.at(context->proxyTailNdx)->getPublicKey() ==
      config::PeerServiceConfig::getInstance().getMyPublicKey()) {
          // logger::info("sumeragi")    <<  "I will send event to " <<
      context->validatingPeers.at(context->proxyTailNdx)->getIP();
          connection::iroha::SumeragiImpl::Verify::send(context->validatingPeers.at(context->proxyTailNdx)->getIP(),
      std::move(event)); // Think In Process } else {
          //logger::info("sumeragi")    <<  "Send All! sig:["       <<
      transaction_validator::countValidSignatures(event) << "]";
          connection::iroha::SumeragiImpl::Verify::sendAll(std::move(event)); //
      TODO: Think In Process
      }

      setAwkTimer(3000, [&](){
          // if
      (!merkle_transaction_repository::leafExists(detail::hash(event.transaction())))
      { panic(event);
          // }
      });
      */
    }
  }
}

/**
 *
 * For example, given:
 * if f := 1, then
 *  _________________    _________________
 * /        A        \  /        B        \
 * |---|  |---|  |---|  |---|  |---|  |---|
 * | 0 |--| 1 |--| 2 |--| 3 |--| 4 |--| 5 |
 * |---|  |---|  |---|  |---|  |---|  |---|,
 *
 * if 2f+1 signature are not received within the timer's limit, then
 * the set of considered validators, A, is expanded by f (e.g., by 1 in the
 * example below):
 *  ________________________    __________
 * /           A            \  /    B     \
 * |---|  |---|  |---|  |---|  |---|  |---|
 * | 0 |--| 1 |--| 2 |--| 3 |--| 4 |--| 5 |
 * |---|  |---|  |---|  |---|  |---|  |---|.
 */
void panic(std::unique_ptr<ConsensusEvent> event) {
  context->panicCount++;  // TODO: reset this later
  auto broadcastStart =
      2 * context->maxFaulty + 1 + context->maxFaulty * context->panicCount;
  auto broadcastEnd = broadcastStart + context->maxFaulty;

  // Do some bounds checking
  if (broadcastStart > context->numValidatingPeers - 1) {
    broadcastStart = context->numValidatingPeers - 1;
  }

  if (broadcastEnd > context->numValidatingPeers - 1) {
    broadcastEnd = context->numValidatingPeers - 1;
  }

  logger::info("sumeragi") << "broadcastEnd:" << broadcastEnd;
  logger::info("sumeragi") << "broadcastStart:" << broadcastStart;
  // WIP issue hash event
  // connection::sendAll(event->transaction().hash()); //TODO: change this to
  // only broadcast to peer range between broadcastStart and broadcastEnd
}

void setAwkTimer(int const sleepMillisecs,
                 std::function<void(void)> const action) {
  std::thread([action, sleepMillisecs]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepMillisecs));
    action();
  });
}

/**
 * The consensus order is based primarily on the trust scores. If two trust
 * scores are the same, then the order (ascending) of the public keys for the
 * servers are used.
 */
void determineConsensusOrder() {
  // WIP We creat getTrustScore() function. till then circle list
  /*
  std::deque<
          std::unique_ptr<peer::Node>
  > tmp_deq;
  for(int i=1;i<context->validatingPeers.size();i++){
      tmp_deq.push_back(std::move(context->validatingPeers[i]));
  }
  tmp_deq.push_back(std::move(context->validatingPeers[0]));
  context->validatingPeers.clear();
  context->validatingPeers = std::move(tmp_deq);


  std::sort(context->validatingPeers.begin(), context->validatingPeers.end(),
        [](const std::unique_ptr<peer::Node> &lhs,
           const std::unique_ptr<peer::Node> &rhs) {
            return lhs->getTrustScore() > rhs->getTrustScore()
                   || (lhs->getTrustScore() == rhs->getTrustScore()
                       && lhs->getPublicKey() < rhs->getPublicKey());
        }
  );
  logger::info("sumeragi")        <<  "determineConsensusOrder sorted!";
  logger::info("sumeragi")        <<  "determineConsensusOrder myPubkey:"     <<
  context->myPublicKey;

  for(const auto& peer : context->validatingPeers) {
      logger::info("sumeragi")    <<  "determineConsensusOrder PublicKey:"    <<
  peer->getPublicKey(); logger::info("sumeragi")    <<  "determineConsensusOrder
  ip:"           <<  peer->getIP();
  }
  */
  // context->isSumeragi = context->validatingPeers.at(0)->getPublicKey() ==
  // context->myPublicKey;
}

};  // namespace sumeragi