// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "../../../../lib/ofh/receiver/ofh_rx_window_checker.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace ofh;
using namespace std::chrono_literals;

TEST(ofh_rx_window_checker, on_time_packet_counts_one_packet)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=300us.
  rx_window_checker rx_window(true, {2, 9});

  // Create the OTA notification.
  slot_symbol_point ota_slot({1, 1, 1, 1}, 7, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  slot_symbol_point message_slot = ota_slot - 4;
  rx_window.update_rx_window_statistics(message_slot);
  ASSERT_EQ(1, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(0, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, packet_on_the_window_start_count_as_valid)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=300us.
  rx_window_checker rx_window(true, {2, 9});

  // Create the OTA notification.
  slot_symbol_point ota_slot({1, 1, 1, 1}, 7, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  slot_symbol_point message_slot = ota_slot - 2;
  rx_window.update_rx_window_statistics(message_slot);
  ASSERT_EQ(1, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(0, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, packet_on_the_window_end_count_as_valid)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=510us.
  rx_window_checker rx_window(true, {2, 15});

  // Create the OTA notification.
  slot_symbol_point ota_slot({1, 1, 1, 1}, 7, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  slot_symbol_point message_slot = ota_slot - 14;
  rx_window.update_rx_window_statistics(message_slot);
  ASSERT_EQ(1, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(0, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, early_packet_counts_one_packet)
{
  // Create window checker with timing parameters corresponding to Ta4_min=80us, Ta4_max=300us.
  rx_window_checker rx_window(true, {3, 9});

  // Create the OTA notification.
  slot_symbol_point ota_slot({1, 1, 1, 1}, 7, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  slot_symbol_point message_slot = ota_slot - 1;
  rx_window.update_rx_window_statistics(message_slot);
  ASSERT_EQ(0, rx_window.nof_on_time_messages());
  ASSERT_EQ(1, rx_window.nof_early_messages());
  ASSERT_EQ(0, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, late_packet_counts_one_packet)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=300us.
  rx_window_checker rx_window(true, {2, 9});

  // Create the OTA notification.
  slot_symbol_point ota_slot({1, 1, 1, 1}, 7, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  slot_symbol_point message_slot = ota_slot - 28;
  rx_window.update_rx_window_statistics(message_slot);
  ASSERT_EQ(0, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(1, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, window_change_slot_works)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=300us.
  rx_window_checker rx_window(true, {2, 9});

  // Create the OTA notification.
  slot_symbol_point ota_slot({1, 1, 1, 0}, 1, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  slot_symbol_point message_slot({1, 1, 0, 1}, 11, 14);
  rx_window.update_rx_window_statistics(message_slot);
  ASSERT_EQ(1, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(0, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, window_change_sfn_works)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=300us.
  rx_window_checker rx_window(true, {2, 9});

  // Create the OTA notification.
  slot_symbol_point ota_slot({1, 1, 0, 0}, 1, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  slot_symbol_point message_slot({1, 0, 9, 1}, 11, 14);
  rx_window.update_rx_window_statistics(message_slot);
  ASSERT_EQ(1, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(0, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, window_change_sfn_byte_works)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=300us.
  rx_window_checker rx_window(true, {2, 9});

  // Create the OTA notification.
  slot_symbol_point ota_slot({1, 0, 0, 0}, 1, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  slot_symbol_point message_slot({1, 255, 9, 1}, 11, 14);
  rx_window.update_rx_window_statistics(message_slot);
  ASSERT_EQ(1, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(0, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, window_change_sfn_byte_and_message_is_in_sfn_0)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=300us.
  rx_window_checker rx_window(true, {2, 9});

  // Create the OTA notification.
  slot_symbol_point ota_slot({1, 0, 0, 0}, 3, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  slot_symbol_point message_slot({1, 0, 0, 0}, 0, 14);
  rx_window.update_rx_window_statistics(message_slot);
  ASSERT_EQ(1, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(0, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, packet_processed_late_but_received_on_time_counts_as_on_time)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=300us.
  rx_window_checker rx_window(true, {2, 9});

  slot_symbol_point ota_slot({1, 1, 1, 1}, 7, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  // Processed 12 symbols after its symbol (late), received by the kernel 5 symbols before the processing (on time).
  slot_symbol_point  message_slot      = ota_slot - 12;
  std::optional<int> processing_offset = rx_window.update_rx_window_statistics(message_slot, 5);
  ASSERT_TRUE(processing_offset.has_value());
  ASSERT_EQ(12, *processing_offset);
  ASSERT_EQ(1, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(0, rx_window.nof_late_messages());

  // The statistics report the arrival offset at the kernel receive time.
  received_messages_metrics metrics;
  rx_window.collect_metrics(metrics);
  ASSERT_EQ(7, metrics.latest_rx_msg_in_symbols);
  ASSERT_EQ(7, metrics.earliest_rx_msg_in_symbols);
}

TEST(ofh_rx_window_checker, packet_received_late_counts_as_late_regardless_of_processing_time)
{
  // Create window checker with timing parameters corresponding to Ta4_min=50us, Ta4_max=300us.
  rx_window_checker rx_window(true, {2, 9});

  slot_symbol_point ota_slot({1, 1, 1, 1}, 7, 14);
  rx_window.on_new_symbol({ota_slot, {}});

  // Received by the kernel 11 symbols after its symbol, processed one symbol later.
  slot_symbol_point message_slot = ota_slot - 12;
  rx_window.update_rx_window_statistics(message_slot, 1);
  ASSERT_EQ(0, rx_window.nof_on_time_messages());
  ASSERT_EQ(0, rx_window.nof_early_messages());
  ASSERT_EQ(1, rx_window.nof_late_messages());
}

TEST(ofh_rx_window_checker, disabled_checker_reports_no_processing_offset)
{
  rx_window_checker rx_window(false, {2, 9});

  slot_symbol_point ota_slot({1, 1, 1, 1}, 7, 14);
  rx_window.on_new_symbol({ota_slot, {}});
  ASSERT_FALSE(rx_window.update_rx_window_statistics(ota_slot - 4, 1).has_value());
}
