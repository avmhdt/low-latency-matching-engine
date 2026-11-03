#include "lobster_adapter.hpp"

#include <algorithm>
#include <charconv>
#include <optional>

#include "llme/engine.hpp"

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

bool parse_lobster_row(std::string_view line, std::vector<std::int64_t>& out) {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  out.clear();
  while (!line.empty()) {
    std::int64_t value = 0;
    if (!parse_int(next_field(line), value)) {
      return false;
    }
    out.push_back(value);
  }
  return out.size() >= 4 && out.size() % 4 == 0;
}

struct OneStepChecker::Engine {
  explicit Engine(const EngineConfig& cfg) : book(cfg, NullSink{}) {}
  MatchingEngine<LadderLevelStore, NullSink> book;
};

namespace {

constexpr OrderId kEventOrderId = 1;
constexpr OrderId kFirstLevelId = 2;

InMsg limit_order(OrderId id, Side side, Tif tif, Qty qty, Price price) {
  InMsg m;
  m.type = to_wire(MsgType::New);
  m.side = to_wire(side);
  m.tif = to_wire(tif);
  m.qty = qty;
  m.order_id = id;
  m.price = price;
  return m;
}

InMsg cancel_of(OrderId id) {
  InMsg m;
  m.type = to_wire(MsgType::Cancel);
  m.order_id = id;
  return m;
}

// Top-of-book side matches if the engine shows the row's price and size, or is empty when the
// row shows the size-zero dummy.
template <class Book>
bool side_matches(const Book& book, Side side, std::optional<Price> best, std::int64_t price,
                  std::int64_t size) {
  if (size == 0) {
    return !best.has_value();
  }
  return best && *best * kRawPerTick == price &&
         static_cast<std::int64_t>(book.depth_at(side, *best)) == size;
}

}  // namespace

OneStepChecker::OneStepChecker(Price max_ticks)
    : engine_(std::make_unique<Engine>(
          EngineConfig{.price_min = 0, .price_max = max_ticks, .max_orders = 1024})) {}

OneStepChecker::~OneStepChecker() = default;

void OneStepChecker::check(const std::vector<std::int64_t>& previous, const LobsterEvent* event,
                           const std::vector<std::int64_t>& current) {
  auto& book = engine_->book;

  // One aggregate order per visible level; its ID comes from its place in the row.
  const std::size_t levels = previous.size() / 2;
  const auto level_order = [&](Side side, std::int64_t raw_price) -> std::optional<OrderId> {
    const std::size_t first = side == Side::Sell ? 0 : 2;
    for (std::size_t i = first; i + 1 < previous.size(); i += 4) {
      if (previous[i] == raw_price && previous[i + 1] > 0) {
        return kFirstLevelId + i;
      }
    }
    return std::nullopt;
  };
  for (std::size_t i = 0; i + 1 < previous.size(); i += 2) {
    const Side side = i % 4 == 0 ? Side::Sell : Side::Buy;
    const std::int64_t price = previous[i];
    const std::int64_t size = previous[i + 1];
    if (size > 0 && price % kRawPerTick == 0) {
      book.on_message(limit_order(kFirstLevelId + i, side, Tif::Gtc, static_cast<Qty>(size),
                                  price / kRawPerTick));
    }
  }

  if (event != nullptr) {
    const Side side = event->direction == 1 ? Side::Buy : Side::Sell;
    const Price ticks = event->price / kRawPerTick;
    const auto qty = static_cast<Qty>(std::clamp<std::int64_t>(event->size, 0, 0xFFFFFFFFLL));
    if (event->type == kNewLimit && event->price % kRawPerTick == 0) {
      book.on_message(limit_order(kEventOrderId, side, Tif::Gtc, qty, ticks));
    } else if (event->type == kPartialCancel || event->type == kDelete ||
               event->type == kVisibleExec) {
      const auto level = level_order(side, event->price);
      if (!level) {
        ++result_.events_outside_window;
      } else if (event->type == kVisibleExec) {
        book.on_message(limit_order(kEventOrderId, opposite(side), Tif::Ioc, qty, ticks));
      } else {
        const std::uint64_t resting = book.depth_at(side, ticks);
        InMsg modify;
        modify.type = to_wire(MsgType::Modify);
        modify.order_id = *level;
        modify.qty = resting > qty ? static_cast<Qty>(resting - qty) : 0;
        modify.price = ticks;
        book.on_message(modify);
      }
    }
  }

  ++result_.compared;
  const bool agree = side_matches(book, Side::Sell, book.best_ask(), current[0], current[1]) &&
                     side_matches(book, Side::Buy, book.best_bid(), current[2], current[3]);
  result_.agree += agree ? 1U : 0U;

  book.on_message(cancel_of(kEventOrderId));
  for (std::size_t i = 0; i < levels * 2; i += 2) {
    book.on_message(cancel_of(kFirstLevelId + i));
  }
}

}  // namespace llme::workload
