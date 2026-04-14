// SPDX-License-Identifier: MIT
#include "polar_sdk_transport.h"

static bool
polar_sdk_transport_ops_ready(const polar_sdk_transport_connect_ops_t *ops) {
  return ops != 0 && ops->now_ms != 0 && ops->sleep_ms != 0 &&
         ops->start_attempt != 0 && ops->wait_attempt != 0 &&
         ops->cleanup_after_attempt != 0 && ops->on_connect_ready != 0;
}

polar_sdk_transport_connect_result_t polar_sdk_transport_connect_blocking(
    const polar_sdk_connect_policy_t *policy,
    const polar_sdk_transport_connect_ops_t *ops,
    polar_sdk_transport_connect_stats_t *out_stats) {
  if (policy == 0 || !polar_sdk_transport_ops_ready(ops) ||
      policy->timeout_ms == 0) {
    return POLAR_SDK_TRANSPORT_CONNECT_FAILED;
  }

  if (out_stats != 0) {
    out_stats->attempts_total = 0;
    out_stats->backoff_events_total = 0;
  }

  polar_sdk_connect_state_t connect_state;
  polar_sdk_connect_init(&connect_state, ops->now_ms(ops->ctx));

  while (polar_sdk_connect_has_time_left(policy, &connect_state,
                                         ops->now_ms(ops->ctx))) {
    if (out_stats != 0) {
      out_stats->attempts_total += 1;
    }

    if (!ops->start_attempt(ops->ctx)) {
      return POLAR_SDK_TRANSPORT_CONNECT_FAILED;
    }

    uint32_t budget_ms = polar_sdk_connect_attempt_budget_ms(
        policy, &connect_state, ops->now_ms(ops->ctx));
    if (budget_ms == 0) {
      break;
    }

    polar_sdk_transport_attempt_result_t attempt_result =
        ops->wait_attempt(ops->ctx, budget_ms);
    if (attempt_result == POLAR_SDK_TRANSPORT_ATTEMPT_READY) {
      ops->on_connect_ready(ops->ctx);
      return POLAR_SDK_TRANSPORT_CONNECT_OK;
    }

    ops->cleanup_after_attempt(ops->ctx);

    uint32_t backoff_ms = polar_sdk_connect_next_backoff_ms(
        policy, &connect_state, ops->now_ms(ops->ctx));
    if (backoff_ms == 0) {
      break;
    }

    if (out_stats != 0) {
      out_stats->backoff_events_total += 1;
    }
    ops->sleep_ms(ops->ctx, backoff_ms);
  }

  return POLAR_SDK_TRANSPORT_CONNECT_TIMEOUT;
}
