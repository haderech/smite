// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once

#include <fmt/core.h>

#include <noir/codec/scale.h>
#include <noir/common/scope_exit.h>
#include <noir/consensus/db/db.h>
#include <noir/consensus/params.h>
#include <noir/consensus/state.h>
#include <noir/consensus/types.h>
#include <noir/consensus/validator.h>

namespace noir::consensus {

/// \addtogroup consensus
/// \{

/// \brief Store defines the state store interface
/// It is used to retrieve current state and save and load ABCI responses,
/// validators and consensus parameters
class state_store {
public:
  /// \brief Load loads the current state of the blockchain
  /// \return true on success, false otherwise
  virtual bool load(state& st) const = 0;
  /// \brief LoadValidators loads the validator set at a given height
  /// \param[in] height
  /// \param[out] v_set
  /// \return true on success, false otherwise
  virtual bool load_validators(int64_t height, validator_set& v_set) const = 0;
  /// \brief LoadABCIResponses loads the abciResponse for a given height
  /// \param[in] height
  /// \return true on success, false otherwise
  virtual bool load_abci_responses(int64_t height /* , abci_response& abci_rsp */) const = 0;
  /// \brief LoadConsensusParams loads the consensus params for a given height
  /// \param[in] height
  /// \param[out] cs_param
  /// \return true on success, false otherwise
  virtual bool load_consensus_params(int64_t height, consensus_params& cs_param) const = 0;
  /// \brief Save overwrites the previous state with the updated one
  /// \param[in] st
  /// \return true on success, false otherwise
  virtual bool save(const state& st) = 0;
  /// \brief SaveABCIResponses saves ABCIResponses for a given height
  /// \param[in] height
  /// \return true on success, false otherwise
  virtual bool save_abci_responses(int64_t height /* , const abci_response& abci_rsp */) = 0;
  /// \brief SaveValidatorSet saves the validator set at a given height
  /// \param[in] lower_height
  /// \param[in] upper_height
  /// \param[in] v_set
  /// \return true on success, false otherwise
  virtual bool save_validator_sets(int64_t lower_height, int64_t upper_height, const validator_set& v_set) = 0;
  /// \brief Bootstrap is used for bootstrapping state when not starting from a initial height.
  /// \param[in] st
  /// \return true on success, false otherwise
  virtual bool bootstrap(const state& st) = 0;
  /// \brief PruneStates takes the height from which to prune up to (exclusive)
  /// \param[in] height
  /// \return true on success, false otherwise
  virtual bool prune_states(int64_t height) = 0;
};

class db_store : public state_store {
private:
  // TEMP struct for encoding
  struct validators_info {
    int64_t last_height_changed;
    std::optional<validator_set> v_set;
  };
  struct consensus_params_info {
    int64_t last_height_changed;
    std::optional<consensus_params> cs_param;
  };
  struct response_deliver_tx {
    noir::p2p::bytes data; // dummy
  };

public:
  explicit db_store(const std::string& db_type = "simple")
    : db_(new simple_db<noir::p2p::bytes, noir::p2p::bytes>),
      state_key_(noir::codec::scale::encode(static_cast<char>(prefix::state))) {}

  db_store(db_store&& other) noexcept : db_(std::move(other.db_)) {}

  bool load(state& st) const override {
    return load_internal(st);
  }

  bool load_validators(int64_t height, validator_set& v_set) const override {
    validators_info v_info{};
    if (!load_validators_info(height, v_info)) {
      return false;
    }

    if (v_info.v_set == std::nullopt) {
      int64_t last_stored_height = last_stored_height_for(height, v_info.last_height_changed);

      if (bool ret = load_validators_info(last_stored_height, v_info); (!ret) || (v_info.v_set == std::nullopt)) {
        return false;
      }
      v_info.v_set->increment_proposer_priority(
        static_cast<int32_t>(height - v_info.last_height_changed)); // safe_convert_int?
    }
    v_set = *v_info.v_set;

    return true;
  }

  bool load_abci_responses(int64_t height /* , abci_response& rsp */) const override {
    return load_abci_response_internal(height /*, rsp */);
  }

  bool load_consensus_params(int64_t height, consensus_params& cs_param) const override {
    consensus_params_info cs_param_info{};
    if (auto ret = load_consensus_params_info(height, cs_param_info); !ret) {
      return false;
    }
    if (cs_param_info.cs_param == std::nullopt) {
      if (auto ret = load_consensus_params_info(cs_param_info.last_height_changed, cs_param_info);
          !ret || cs_param_info.cs_param == std::nullopt) {
        return false;
      }
    }
    cs_param = *cs_param_info.cs_param;

    return true;
  }

  // Save persists the State, the ValidatorsInfo, and the ConsensusParamsInfo to the database.
  // This flushes the writes (e.g. calls SetSync).
  bool save(const state& st) override {
    return save_internal(st);
  }

  bool save_abci_responses(int64_t height /* , const abci_response& rsp */) override {
    return save_abci_responses_internal(height /* , rsp */);
  }

  bool save_validator_sets(int64_t lower_height, int64_t upper_height, const validator_set& v_set) override {
    auto batch = db_->new_batch();
    auto batch_close = make_scoped_exit<std::function<void()>>([&batch]() { batch->close(); });

    for (auto height = lower_height; height <= upper_height; ++height) {
      if (!save_validators_info(height, lower_height, v_set, batch.get())) {
        batch->close();
        return false;
      }
    }

    bool ret = batch->write_sync();
    return ret;
  }

  bool bootstrap(const state& st) override {
    return bootstrap_internal(st);
  }

  bool prune_states(int64_t retain_height) override {
    if (retain_height <= 0) {
      return false;
    }

    if (!prune_consensus_param(retain_height)) {
      return false;
    }
    if (!prune_validator_sets(retain_height)) {
      return false;
    }
    if (!prune_abci_response(retain_height)) {
      return false;
    }
    return true;
  }

private:
  enum class prefix : char {
    validators = 5,
    consensus_params = 6,
    abci_response = 7,
    state = 8,
  };
  static constexpr int val_set_checkpoint_interval = 100000;
  std::unique_ptr<db<noir::p2p::bytes, noir::p2p::bytes>> db_;
  noir::p2p::bytes state_key_;

  template<prefix key_prefix>
  static noir::p2p::bytes encode_key(int64_t height) {
    std::string s = static_cast<char>(key_prefix) + fmt::format("{:08x}", height);
    return {s.begin(), s.end()};
  }

  bool save_internal(const state& st) {
    auto batch = db_->new_batch();
    auto batch_close = make_scoped_exit<std::function<void()>>([&batch]() { batch->close(); });

    auto next_height = st.last_block_height + 1;
    if (next_height == 1) {
      next_height = st.initial_height;
      if (!save_validators_info(next_height, next_height, st.validators, batch.get())) {
        return false;
      }
    }
    if (!save_validators_info(next_height + 1, st.last_height_validators_changed, st.next_validators, batch.get())) {
      return false;
    }

    if (!save_consensus_params_info(
          next_height, st.last_height_consensus_params_changed, st.consensus_params, batch.get())) {
      return false;
    }

    if (!batch->set(state_key_, encode_state(st))) {
      return false;
    }
    return batch->write_sync();
  }

  bool bootstrap_internal(const state& st) {
    auto batch = db_->new_batch();
    auto batch_close = make_scoped_exit<std::function<void()>>([&batch]() { batch->close(); });
    auto height = st.last_block_height + 1;
    if (height == 1) {
      height = st.initial_height;
    } else if (!st.last_validators.validators.empty()) { // height > 1, can height < 0 ?
      if (!save_validators_info(height, height, st.validators, batch.get())) {
        return false;
      }
    }
    if (!save_validators_info(height + 1, height + 1, st.validators, batch.get())) {
      return false;
    }
    if (!save_consensus_params_info(
          height, st.last_height_consensus_params_changed, st.consensus_params, batch.get())) {
      return false;
    }
    if (!batch->set(state_key_, encode_state(st))) {
      return false;
    }
    return batch->write_sync();
  }

  bool load_internal(state& st) const {
    p2p::bytes buf;
    if (auto ret = db_->get(state_key_, buf); !ret || buf.empty()) {
      return false;
    }
    decode_state(buf, st);
    return true;
  }

  static bool save_validators_info(int64_t height, int64_t last_height_changed, const validator_set& v_set,
    db<noir::p2p::bytes, noir::p2p::bytes>::batch* batch) {
    if (last_height_changed > height) {
      return false;
    }
    validators_info v_info{
      .last_height_changed = last_height_changed,
      .v_set = (height == last_height_changed || height % val_set_checkpoint_interval == 0)
        ? std::optional<validator_set>(v_set)
        : std::nullopt,
    };
    auto buf = noir::codec::scale::encode(v_info);
    return batch->set(encode_key<prefix::validators>(height), buf);
  } // namespace noir::consensus

  bool load_validators_info(int64_t height, validators_info& v_info) const {
    noir::p2p::bytes buf;
    if (auto ret = db_->get(encode_key<prefix::validators>(height), buf); !ret || buf.empty()) {
      return false;
    }
    v_info = noir::codec::scale::decode<validators_info>(buf);
    return true;
  }

  static int64_t last_stored_height_for(int64_t height, int64_t last_height_changed) {
    int64_t checkpoint_height = height - height % val_set_checkpoint_interval;
    return std::max(checkpoint_height, last_height_changed);
  }

  static bool save_consensus_params_info(int64_t next_height, int64_t change_height, const consensus_params& cs_params,
    db<noir::p2p::bytes, noir::p2p::bytes>::batch* batch) {
    consensus_params_info cs_param_info{
      .last_height_changed = change_height,
      .cs_param = (change_height == next_height) ? std::optional<consensus_params>(cs_params) : std::nullopt,
    };
    auto buf = noir::codec::scale::encode(cs_param_info);
    return batch->set(encode_key<prefix::consensus_params>(next_height), buf);
  }

  bool load_consensus_params_info(int64_t height, consensus_params_info& cs_param_info) const {
    noir::p2p::bytes buf;
    if (auto ret = db_->get(encode_key<prefix::consensus_params>(height), buf); !ret || buf.empty()) {
      return false;
    }
    cs_param_info = noir::codec::scale::decode<consensus_params_info>(buf);
    return true;
  }

  bool save_abci_responses_internal(int64_t height /* , abci_response& rsp */) {
    std::vector<response_deliver_tx> txs{};
    //    std::for_each(rsp.deliver_txs.begin(), rsp.deliver_txs.end(), [&](const auto& t){
    //      if (t.size() !=0) {
    //        txs.push_back(t);
    //      }
    //    });
    auto buf = noir::codec::scale::encode(txs);
    return db_->set(encode_key<prefix::abci_response>(height), buf);
  }

  bool load_abci_response_internal(int64_t height /* , abci_response& rsp */) const {
    noir::p2p::bytes buf;
    if (auto ret = db_->get(encode_key<prefix::abci_response>(height), buf); !ret || buf.empty()) {
      return false;
    }

    auto txs = noir::codec::scale::decode<std::vector<response_deliver_tx>>(buf);
    //    rsp.deliver_txs = txs;
    return true;
  }

  bool prune_consensus_param(int64_t retain_height) {
    consensus_params_info cs_info{};
    if (!load_consensus_params_info(retain_height, cs_info)) {
      return false;
    }
    if (cs_info.cs_param == std::nullopt) {
      if (auto ret = load_consensus_params_info(cs_info.last_height_changed, cs_info);
          !ret || (cs_info.cs_param == std::nullopt)) {
        return false;
      }

      if (!prune_range<prefix::consensus_params>(cs_info.last_height_changed + 1, retain_height)) {
        return false;
      }
    }

    return prune_range<prefix::consensus_params>(1, cs_info.last_height_changed);
  }

  bool prune_validator_sets(int64_t retain_height) {
    validators_info val_info{};
    if (!load_validators_info(retain_height, val_info)) {
      return false;
    }
    int64_t last_recorded_height = last_stored_height_for(retain_height, val_info.last_height_changed);
    if (val_info.v_set == std::nullopt) {
      if (auto ret = load_validators_info(last_recorded_height, val_info); !ret || (val_info.v_set == std::nullopt)) {
        return false;
      }

      if (last_recorded_height < retain_height) {
        if (!prune_range<prefix::validators>(last_recorded_height + 1, retain_height)) {
          return false;
        }
      }
    }

    return prune_range<prefix::validators>(1, last_recorded_height);
  }

  bool prune_abci_response(int64_t height) {
    return prune_range<prefix::abci_response>(1, height);
  }

  template<prefix key_prefix>
  bool prune_range(int64_t start_, int64_t end_) {
    p2p::bytes start;
    p2p::bytes end;
    start = encode_key<key_prefix>(start_);
    end = encode_key<key_prefix>(end_ - 1);
    do {
      auto batch = db_->new_batch();
      if (!reverse_batch_delete(batch.get(), start, end, end)) {
        return false;
      }
      bool fin = (start == end);
      bool ret = (fin) ? batch->write_sync() : batch->write();

      if (!batch->close() || !ret) {
        return false;
      }
      if (fin) {
        break;
      }
    } while (true); // TODO: change to safer

    return true;
  }

  bool reverse_batch_delete(
    db<p2p::bytes, p2p::bytes>::batch* batch, const p2p::bytes& start, const p2p::bytes& end, p2p::bytes& new_end) {
    auto db_it = db_->get_reverse_iterator(start, end);
    size_t size = 0;
    for (auto it = db_it.begin(); it != db_it.end(); ++it) { // TODO: need safe exit
      if (!batch->del(it.key())) {
        new_end = end;
        return false;
      }
      if (++size == 1000) {
        new_end = it.key();
        return true;
      }
    }
    new_end = start;
    return true;
  }

  // TEMP
  static size_t encode_state_size(const state& st) {
    noir::codec::scale::datastream<size_t> ds;
    ds << st.version << st.chain_id << st.initial_height << st.last_block_height;
    // ds << st.last_block_id;
    ds << st.last_block_time << st.next_validators << st.validators << st.last_validators
       << st.last_height_validators_changed << st.consensus_params << st.last_height_consensus_params_changed
       << st.last_result_hash << st.app_hash;
    return ds.tellp();
  }

  static p2p::bytes encode_state(const state& st) {
    auto buf = p2p::bytes(encode_state_size(st));

    noir::codec::scale::datastream<char> ds(buf);
    ds << st.version << st.chain_id << st.initial_height << st.last_block_height;
    // ds << st.last_block_id;
    ds << st.last_block_time << st.next_validators << st.validators << st.last_validators
       << st.last_height_validators_changed << st.consensus_params << st.last_height_consensus_params_changed
       << st.last_result_hash << st.app_hash;

    return buf;
  }

  static void decode_state(const p2p::bytes& buf, state& st) {
    noir::codec::scale::datastream<char> ds(const_cast<p2p::bytes&>(buf));
    ds >> st.version >> st.chain_id >> st.initial_height >> st.last_block_height;
    // ds >> st.last_block_id;
    ds >> st.last_block_time >> st.next_validators >> st.validators >> st.last_validators >>
      st.last_height_validators_changed >> st.consensus_params >> st.last_height_consensus_params_changed >>
      st.last_result_hash >> st.app_hash;
  }
};

/// }

} // namespace noir::consensus
