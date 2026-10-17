#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace render {
template <typename SampleType> class WeightedReservoir {
public:
    void Update(const SampleType& sample, double weight, double target, double random01) {
        Validate(weight, target, random01);
        Add(sample, weight, target, 1, random01);
    }

    void Merge(const WeightedReservoir& other, double targetAtReceiver, double random01) {
        Validate(0, targetAtReceiver, random01);
        // Reevaluate the selected target at the receiver while preserving the source stream mass.
        const double weight = other.sample_ ? targetAtReceiver * (other.weightSum_ / other.selectedTarget_) : 0;
        if (!std::isfinite(weight)) throw std::invalid_argument("reservoir merge weight is not finite");
        if (other.sample_) Add(*other.sample_, weight, targetAtReceiver, other.members_, random01);
        else AddEmpty(other.members_);
    }

    void ClampHistory(std::uint64_t maximumMembers) {
        if (members_ <= maximumMembers) return;
        if (maximumMembers == 0) { Clear(); return; }
        // Scaling mass with M keeps the estimator unchanged when limiting temporal history.
        weightSum_ *= static_cast<double>(maximumMembers) / static_cast<double>(members_);
        members_ = maximumMembers;
    }

    void Clear() { sample_.reset(); weightSum_ = 0; selectedTarget_ = 0; members_ = 0; }
    bool HasSample() const { return sample_.has_value(); }
    const SampleType& Sample() const {
        if (!sample_) throw std::logic_error("reservoir has no selected sample");
        return *sample_;
    }
    std::uint64_t Members() const { return members_; }
    double WeightSum() const { return weightSum_; }
    double SelectedTarget() const { return selectedTarget_; }
    double Normalization() const {
        return sample_ && members_ ? (weightSum_ / static_cast<double>(members_)) / selectedTarget_ : 0;
    }

private:
    std::optional<SampleType> sample_; // Selected representative of the weighted candidate stream.
    double weightSum_{}; // Sum of proposal-corrected candidate weights, including merged stream mass.
    double selectedTarget_{}; // Unnormalized target density evaluated at the selected sample.
    std::uint64_t members_{}; // Number of represented candidates, including zero-weight candidates.

    static void Validate(double weight, double target, double random01) {
        if (!std::isfinite(weight) || weight < 0) throw std::invalid_argument("reservoir weight must be finite and nonnegative");
        if (!std::isfinite(target) || target < 0 || (weight > 0 && target == 0))
            throw std::invalid_argument("positive reservoir weight requires a finite positive target");
        if (!std::isfinite(random01) || random01 < 0 || random01 >= 1)
            throw std::invalid_argument("reservoir random value must be in [0, 1)");
    }

    void AddEmpty(std::uint64_t count) {
        if (count > std::numeric_limits<std::uint64_t>::max() - members_)
            throw std::overflow_error("reservoir candidate count overflow");
        members_ += count;
    }

    void Add(const SampleType& candidate, double weight, double target, std::uint64_t count, double random01) {
        if (count > std::numeric_limits<std::uint64_t>::max() - members_)
            throw std::overflow_error("reservoir candidate count overflow");
        const double sum = weightSum_ + weight;
        if (!std::isfinite(sum)) throw std::overflow_error("reservoir weight sum overflow");
        if (weight > 0 && (!sample_ || random01 < weight / sum)) {
            sample_ = candidate;
            selectedTarget_ = target;
        }
        weightSum_ = sum;
        members_ += count;
    }
};
}
