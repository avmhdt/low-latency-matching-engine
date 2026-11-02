#include "generator.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace llme::workload {

namespace {

constexpr std::uint64_t kSplitmixGamma = 0x9E3779B97F4A7C15ULL;
constexpr double kTwoTo64 = 18446744073709551616.0;
constexpr std::uint32_t kNearTouchSamples = 8;
constexpr std::uint32_t kMaxGeometricTrials = 1U << 16U;

std::uint64_t splitmix64(std::uint64_t& x) noexcept {
  x += kSplitmixGamma;
  std::uint64_t z = x;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

// x^(n/16) from sqrt and products only, so that it is exact-rounding reproducible everywhere.
double pow_sixteenths(double x, std::uint32_t n) noexcept {
  double root = x;
  for (int i = 0; i < 4; ++i) {
    root = std::sqrt(root);
  }
  double out = 1.0;
  for (std::uint32_t i = 0; i < n; ++i) {
    out *= root;
  }
  return out;
}

// Number of failures before the first success of a Bernoulli trial, capped.
std::uint32_t geometric(Rng& rng, std::uint64_t threshold) noexcept {
  std::uint32_t failures = 0;
  while (failures < kMaxGeometricTrials && !rng.chance(threshold)) {
    ++failures;
  }
  return failures;
}

}  // namespace

Rng::Rng(std::uint64_t seed) noexcept {
  for (auto& word : state_) {
    word = splitmix64(seed);
  }
}

std::uint64_t Rng::next() noexcept {
  const std::uint64_t result = std::rotl(state_[1] * 5, 7) * 9;
  const std::uint64_t t = state_[1] << 17U;
  state_[2] ^= state_[0];
  state_[3] ^= state_[1];
  state_[1] ^= state_[2];
  state_[0] ^= state_[3];
  state_[2] ^= t;
  state_[3] = std::rotl(state_[3], 45);
  return result;
}

std::uint64_t Rng::below(std::uint64_t bound) noexcept {
  // Multiply-shift on the high word. The tiny modulo bias does not matter for test flow, and it
  // keeps one draw per call, so the stream never depends on a rejection loop.
  __extension__ using Wide = unsigned __int128;
  return static_cast<std::uint64_t>((static_cast<Wide>(next()) * bound) >> 64U);
}

std::uint64_t probability_threshold(double p) noexcept {
  if (p <= 0.0) {
    return 0;
  }
  if (p >= 1.0) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(p * kTwoTo64);
}

Generator::Generator(const Profile& profile, std::uint64_t seed)
    : profile_(profile),
      rng_(seed),
      shadow_(profile.engine),
      mid_(profile.mid_start),
      floor_(profile.price_floor.value_or(profile.engine.price_min)),
      ceil_(profile.price_ceil.value_or(profile.engine.price_max)),
      mid_step_threshold_(
          probability_threshold(profile.mid_walk_sigma * profile.mid_walk_sigma / 1000.0)),
      cross_threshold_(probability_threshold(profile.cross_prob)),
      price_geometric_threshold_(probability_threshold(profile.price_p)),
      size_geometric_threshold_(probability_threshold(1.0 / profile.size_mean)) {
  const Mix& mix = profile.mix;
  const std::array<double, 6> shares = {mix.new_limit, mix.cancel, mix.modify,
                                        mix.ioc,       mix.fok,    mix.market};
  double running = 0.0;
  for (std::size_t i = 0; i < shares.size(); ++i) {
    running += shares[i];
    mix_cumulative_[i] = probability_threshold(running);
  }
  const double modify_total =
      profile.modify_reduce + profile.modify_increase + profile.modify_reprice;
  modify_cumulative_[0] = probability_threshold(profile.modify_reduce / modify_total);
  modify_cumulative_[1] =
      probability_threshold((profile.modify_reduce + profile.modify_increase) / modify_total);

  if (profile.price_dist == PriceDist::PowerLaw) {
    const auto sixteenths = static_cast<std::uint32_t>(std::lround(profile.price_alpha * 16.0));
    std::vector<double> weights(profile.price_max_offset + 1);
    double total = 0.0;
    for (std::size_t k = 0; k < weights.size(); ++k) {
      weights[k] = 1.0 / pow_sixteenths(static_cast<double>(k + 1), sixteenths);
      total += weights[k];
    }
    power_law_cdf_.reserve(weights.size());
    double cumulative = 0.0;
    for (const double w : weights) {
      cumulative += w;
      power_law_cdf_.push_back(probability_threshold(cumulative / total));
    }
    power_law_cdf_.back() = std::numeric_limits<std::uint64_t>::max();
  }
}

InMsg Generator::next() {
  advance_mid();
  const InMsg m = make_message();
  apply(m);
  ++emitted_;
  return m;
}

InMsg Generator::make_message() {
  if (profile_.burst_enabled && emitted_ != 0 && emitted_ % profile_.burst_every == 0) {
    burst_left_ = profile_.burst_length;
  }
  if (burst_left_ > 0) {
    --burst_left_;
    return make_cancel(profile_.cancel_target);
  }

  const std::uint64_t draw = rng_.next();
  if (draw < mix_cumulative_[0]) {
    return make_new_limit();
  }
  if (draw < mix_cumulative_[1]) {
    return make_cancel(profile_.cancel_target);
  }
  if (draw < mix_cumulative_[2]) {
    return make_modify();
  }
  if (draw < mix_cumulative_[3]) {
    return make_aggressive(Tif::Ioc);
  }
  if (draw < mix_cumulative_[4]) {
    return make_aggressive(Tif::Fok);
  }
  if (draw < mix_cumulative_[5]) {
    return make_market();
  }
  return make_invalid();
}

InMsg Generator::make_new_limit() {
  if (shadow_.live_orders() >= profile_.target_live && !dense_.empty()) {
    return make_cancel(profile_.cancel_target);
  }
  InMsg m;
  const Side side = random_side();
  m.type = to_wire(MsgType::New);
  m.side = to_wire(side);
  m.ord_type = to_wire(OrdType::Limit);
  m.tif = to_wire(Tif::Gtc);
  m.qty = random_size();
  m.order_id = fresh_id();
  m.price = rng_.chance(cross_threshold_) ? marketable_price(side) : passive_price(side);
  m.trader_id = 1 + static_cast<std::uint32_t>(rng_.below(profile_.traders));
  return m;
}

InMsg Generator::make_aggressive(Tif tif) {
  InMsg m;
  const Side side = random_side();
  m.type = to_wire(MsgType::New);
  m.side = to_wire(side);
  m.ord_type = to_wire(OrdType::Limit);
  m.tif = to_wire(tif);
  m.qty = random_size();
  m.order_id = fresh_id();
  m.price = rng_.below(4) == 0 ? passive_price(side) : marketable_price(side);
  m.trader_id = 1 + static_cast<std::uint32_t>(rng_.below(profile_.traders));
  return m;
}

InMsg Generator::make_market() {
  InMsg m;
  m.type = to_wire(MsgType::New);
  m.side = to_wire(random_side());
  m.ord_type = to_wire(OrdType::Market);
  m.tif = to_wire(Tif::Ioc);
  m.qty = random_size();
  m.order_id = fresh_id();
  m.trader_id = 1 + static_cast<std::uint32_t>(rng_.below(profile_.traders));
  return m;
}

InMsg Generator::make_cancel(CancelTarget target) {
  if (dense_.empty()) {
    return make_new_limit();
  }
  InMsg m;
  m.type = to_wire(MsgType::Cancel);
  m.order_id = slots_[pick_target(target)].id;
  return m;
}

InMsg Generator::make_modify() {
  if (dense_.empty()) {
    return make_new_limit();
  }
  const Tracked& order = slots_[pick_target(CancelTarget::Uniform)];
  InMsg m;
  m.type = to_wire(MsgType::Modify);
  m.order_id = order.id;
  m.price = order.price;
  m.qty = order.qty;
  const std::uint64_t draw = rng_.next();
  if (draw < modify_cumulative_[0]) {
    m.qty = rng_.below(50) == 0 ? 0 : 1 + static_cast<Qty>(rng_.below(order.qty));
  } else if (draw < modify_cumulative_[1]) {
    m.qty = order.qty + std::min(random_size(), std::numeric_limits<Qty>::max() - order.qty);
  } else {
    m.price =
        rng_.chance(cross_threshold_) ? marketable_price(order.side) : passive_price(order.side);
  }
  return m;
}

InMsg Generator::make_invalid() {
  InMsg m;
  m.type = to_wire(MsgType::New);
  m.side = to_wire(random_side());
  m.qty = random_size();
  m.order_id = fresh_id();
  m.price = passive_price(static_cast<Side>(m.side));
  switch (rng_.below(9)) {
    case 0:
      m.qty = 0;
      break;
    case 1:
      m.price = rng_.below(2) == 0 ? profile_.engine.price_min - 1 - std::int64_t{1}
                                   : profile_.engine.price_max + 1;
      break;
    case 2:
      if (!dense_.empty()) {
        m.order_id = slots_[pick_target(CancelTarget::Uniform)].id;
      }
      break;
    case 3:
      m.type =
          rng_.below(2) == 0 ? std::uint8_t{0} : static_cast<std::uint8_t>(4 + rng_.below(252));
      break;
    case 4:
      m.side = static_cast<std::uint8_t>(2 + rng_.below(254));
      break;
    case 5:
      m.ord_type = static_cast<std::uint8_t>(2 + rng_.below(254));
      break;
    case 6:
      m.tif = static_cast<std::uint8_t>(3 + rng_.below(253));
      break;
    case 7:
      m.type = to_wire(MsgType::Cancel);
      m.order_id = 1 + rng_.below(last_id_ + 16);
      break;
    default:
      m.type = to_wire(MsgType::Modify);
      m.order_id = 1 + rng_.below(last_id_ + 16);
      m.price = rng_.below(4) == 0 ? profile_.engine.price_max + 1 : m.price;
      break;
  }
  return m;
}

Side Generator::random_side() noexcept {
  return (rng_.next() >> 63U) == 0 ? Side::Buy : Side::Sell;
}

Qty Generator::random_size() noexcept {
  if (profile_.size_dist == SizeDist::Uniform) {
    const std::uint64_t span = std::uint64_t{profile_.size_max} - profile_.size_min + 1;
    return profile_.size_min + static_cast<Qty>(rng_.below(span));
  }
  return 1 + geometric(rng_, size_geometric_threshold_);
}

std::uint32_t Generator::random_offset() noexcept {
  switch (profile_.price_dist) {
    case PriceDist::Uniform:
      return static_cast<std::uint32_t>(rng_.below(std::uint64_t{profile_.price_max_offset} + 1));
    case PriceDist::PowerLaw: {
      const auto hit = std::upper_bound(power_law_cdf_.begin(), power_law_cdf_.end(), rng_.next());
      return static_cast<std::uint32_t>(hit - power_law_cdf_.begin());
    }
    case PriceDist::Geometric:
      break;
  }
  return std::min(geometric(rng_, price_geometric_threshold_), profile_.price_max_offset);
}

Price Generator::clamp_price(Price p) const noexcept { return std::clamp(p, floor_, ceil_); }

Price Generator::passive_price(Side side) noexcept {
  const Price offset = random_offset();
  return clamp_price(side == Side::Buy ? mid_ - profile_.half_spread - offset
                                       : mid_ + profile_.half_spread + offset);
}

Price Generator::marketable_price(Side side) noexcept {
  const Side contra = opposite(side);
  const Price offset = random_offset();
  const Price touch = shadow_.best(contra).value_or(
      side == Side::Buy ? mid_ + profile_.half_spread : mid_ - profile_.half_spread);
  return clamp_price(side == Side::Buy ? touch + offset : touch - offset);
}

std::uint32_t Generator::pick_target(CancelTarget target) {
  switch (target) {
    case CancelTarget::Oldest:
      return oldest_;
    case CancelTarget::Newest:
      return newest_;
    case CancelTarget::NearTouch: {
      std::uint32_t best = dense_[rng_.below(dense_.size())];
      for (std::uint32_t i = 1; i < kNearTouchSamples; ++i) {
        const std::uint32_t candidate = dense_[rng_.below(dense_.size())];
        const Price distance = std::abs(slots_[candidate].price - mid_);
        if (distance < std::abs(slots_[best].price - mid_)) {
          best = candidate;
        }
      }
      return best;
    }
    case CancelTarget::Uniform:
      break;
  }
  return dense_[rng_.below(dense_.size())];
}

void Generator::advance_mid() noexcept {
  if (rng_.chance(mid_step_threshold_)) {
    const Price step = (rng_.next() >> 63U) == 0 ? -1 : 1;
    mid_ = std::clamp(mid_ + step, floor_ + profile_.half_spread, ceil_ - profile_.half_spread);
  }
}

void Generator::apply(const InMsg& m) {
  out_.clear();
  shadow_.on_message(m, out_);

  Qty new_order_leaves = m.qty;
  bool rejected = false;
  for (const OutMsg& r : out_) {
    if (r.type == to_wire(OutType::Reject)) {
      rejected = true;
    } else if (r.type == to_wire(OutType::Fill)) {
      if (r.order_id == m.order_id && m.type == to_wire(MsgType::New)) {
        new_order_leaves = r.leaves;
      }
      if (const auto found = slot_of_.find(r.order_id); found != slot_of_.end()) {
        if (r.leaves == 0) {
          untrack(found->second);
        } else {
          slots_[found->second].qty = r.leaves;
        }
      }
    } else if (r.type == to_wire(OutType::Canceled)) {
      if (const auto found = slot_of_.find(r.order_id); found != slot_of_.end()) {
        untrack(found->second);
      }
    } else if (r.type == to_wire(OutType::Modified)) {
      if (const auto found = slot_of_.find(r.order_id); found != slot_of_.end()) {
        update_tracked(found->second, r.price, r.qty, (r.flags & kFlagPriorityLost) != 0);
      }
    }
  }

  const bool rests = m.type == to_wire(MsgType::New) && m.ord_type == to_wire(OrdType::Limit) &&
                     m.tif == to_wire(Tif::Gtc);
  if (rests && !rejected && new_order_leaves > 0) {
    track(m, new_order_leaves);
  }
}

void Generator::track(const InMsg& m, Qty leaves) {
  std::uint32_t slot = 0;
  if (free_slots_.empty()) {
    slot = static_cast<std::uint32_t>(slots_.size());
    slots_.emplace_back();
  } else {
    slot = free_slots_.back();
    free_slots_.pop_back();
  }
  Tracked& order = slots_[slot];
  order.id = m.order_id;
  order.price = m.price;
  order.qty = leaves;
  order.side = static_cast<Side>(m.side);
  order.dense_pos = static_cast<std::uint32_t>(dense_.size());
  dense_.push_back(slot);
  slot_of_[m.order_id] = slot;
  link_newest(slot);
}

void Generator::untrack(std::uint32_t slot) {
  const Tracked& order = slots_[slot];
  const std::uint32_t moved = dense_.back();
  dense_[order.dense_pos] = moved;
  slots_[moved].dense_pos = order.dense_pos;
  dense_.pop_back();
  unlink_age(slot);
  slot_of_.erase(order.id);
  free_slots_.push_back(slot);
}

void Generator::update_tracked(std::uint32_t slot, Price price, Qty qty, bool to_tail) {
  slots_[slot].price = price;
  slots_[slot].qty = qty;
  if (to_tail) {
    unlink_age(slot);
    link_newest(slot);
  }
}

void Generator::link_newest(std::uint32_t slot) {
  slots_[slot].older = newest_;
  slots_[slot].newer = kNil;
  if (newest_ != kNil) {
    slots_[newest_].newer = slot;
  } else {
    oldest_ = slot;
  }
  newest_ = slot;
}

void Generator::unlink_age(std::uint32_t slot) {
  const Tracked& order = slots_[slot];
  if (order.older != kNil) {
    slots_[order.older].newer = order.newer;
  } else {
    oldest_ = order.newer;
  }
  if (order.newer != kNil) {
    slots_[order.newer].older = order.older;
  } else {
    newest_ = order.older;
  }
}

}  // namespace llme::workload
