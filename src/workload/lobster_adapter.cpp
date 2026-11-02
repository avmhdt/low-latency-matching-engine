#include "lobster_adapter.hpp"

#include <charconv>

namespace llme::workload {

namespace {

// Takes the text up to the next comma, leaving line after it.
std::string_view next_field(std::string_view& line) noexcept {
  const auto comma = line.find(',');
  const std::string_view field = line.substr(0, comma);
  line = comma == std::string_view::npos ? std::string_view{} : line.substr(comma + 1);
  return field;
}

template <class T>
bool parse_int(std::string_view text, T& out) noexcept {
  if (text.empty()) {
    return false;
  }
  const auto* end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, out);
  return error == std::errc{} && ptr == end;
}

constexpr int kNewLimit = 1;
constexpr int kPartialCancel = 2;
constexpr int kDelete = 3;
constexpr int kVisibleExec = 4;
constexpr int kHiddenExec = 5;
constexpr int kCrossTrade = 6;
constexpr int kHalt = 7;

}  // namespace

bool parse_lobster_message(std::string_view line, LobsterEvent& out) noexcept {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  const std::string_view time = next_field(line);
  if (time.empty() || time.find_first_not_of("0123456789.") != std::string_view::npos) {
    return false;
  }
  const bool ok = parse_int(next_field(line), out.type) &&
                  parse_int(next_field(line), out.order_id) &&
                  parse_int(next_field(line), out.size) && parse_int(next_field(line), out.price) &&
                  parse_int(next_field(line), out.direction);
  return ok && line.empty();
}

bool apply_lobster_event(const LobsterEvent& event, ReplayBuilder& builder) {
  const auto id = static_cast<OrderId>(event.order_id);
  auto& stats = builder.stats();
  if (event.type == kHiddenExec || event.type == kCrossTrade) {
    ++stats.events;
    ++stats.skipped_hidden;
    return true;
  }
  if (event.type == kHalt) {
    ++stats.events;
    ++stats.skipped_halt;
    return true;
  }
  if (event.order_id < 0) {
    return false;
  }
  switch (event.type) {
    case kNewLimit:
      if (event.direction != 1 && event.direction != -1) {
        return false;
      }
      builder.add(id, event.direction == 1 ? Side::Buy : Side::Sell, event.price, event.size);
      return true;
    case kPartialCancel:
      builder.partial_cancel(id, event.size);
      return true;
    case kDelete:
      builder.remove(id);
      return true;
    case kVisibleExec:
      builder.exec(id, event.size);
      return true;
    default:
      return false;
  }
}

bool parse_lobster_top(std::string_view line, TopOfBook& out) noexcept {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return parse_int(next_field(line), out.ask_price) && parse_int(next_field(line), out.ask_size) &&
         parse_int(next_field(line), out.bid_price) && parse_int(next_field(line), out.bid_size);
}

}  // namespace llme::workload
