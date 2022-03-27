// This file is part of NOIR.
//
// Copyright (c) 2022 Haderech Pte. Ltd.
// SPDX-License-Identifier: AGPL-3.0-or-later
//
#pragma once
#include <tendermint/common/common.h>
#include <tendermint/service/service.h>

namespace tendermint::node {

class Node : public service::Service<Node> {
public:
  Node();

  result<void> on_start() noexcept;
  void on_stop() noexcept;
};

} // namespace tendermint::node
