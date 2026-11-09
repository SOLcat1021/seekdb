#include <cmath>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <vector>

namespace {
struct Candidate {
  double squared_distance;
  int64_t key;
  size_t position;

  bool operator<(const Candidate &other) const {
    if (squared_distance != other.squared_distance) {
      return squared_distance < other.squared_distance;
    }
    return key < other.key;
  }
};
}  // namespace

// A self-implemented exact flat scan. Caller owns all buffers.
extern "C" size_t mvd_topk(const float *vectors, const int64_t *keys,
                            const uint8_t *eligible, size_t count, size_t dimension,
                            const float *query, size_t k, size_t *out_positions,
                            double *out_distances) {
  if (vectors == nullptr || keys == nullptr ||
      query == nullptr || out_positions == nullptr || out_distances == nullptr || k == 0) {
    return 0;
  }
  std::priority_queue<Candidate> best;
  for (size_t row = 0; row < count; ++row) {
    if (eligible != nullptr && eligible[row] == 0) {
      continue;
    }
    const float *vector = vectors + row * dimension;
    double squared = 0.0;
    for (size_t col = 0; col < dimension; ++col) {
      const double difference = static_cast<double>(vector[col]) -
                                static_cast<double>(query[col]);
      squared += difference * difference;
    }
    Candidate candidate{squared, keys[row], row};
    if (best.size() < k) {
      best.push(candidate);
    } else if (candidate < best.top()) {
      best.pop();
      best.push(candidate);
    }
  }
  const size_t found = best.size();
  for (size_t position = found; position > 0; --position) {
    const Candidate candidate = best.top();
    best.pop();
    out_positions[position - 1] = candidate.position;
    out_distances[position - 1] = std::sqrt(candidate.squared_distance);
  }
  return found;
}
