#ifndef STAN_MATH_PRIM_SCAL_PROB_NEG_BINOMIAL_2_LPMF_HPP
#define STAN_MATH_PRIM_SCAL_PROB_NEG_BINOMIAL_2_LPMF_HPP

#include <stan/math/prim/meta.hpp>
#include <stan/math/prim/scal/err/check_consistent_sizes.hpp>
#include <stan/math/prim/scal/err/check_positive_finite.hpp>
#include <stan/math/prim/scal/err/check_nonnegative.hpp>
#include <stan/math/prim/scal/fun/size_zero.hpp>
#include <stan/math/prim/scal/fun/multiply_log.hpp>
#include <stan/math/prim/scal/fun/digamma.hpp>
#include <stan/math/prim/scal/fun/lgamma.hpp>
#include <stan/math/prim/scal/fun/binomial_coefficient_log.hpp>
#include <stan/math/prim/scal/fun/value_of.hpp>
#include <stan/math/prim/scal/prob/poisson_lpmf.hpp>
#include <cmath>

namespace stan {
namespace math {

namespace internal {
  //Exposing to let me us this in tests
  //The current tests fail when the cutoff is 1e8 and pass with 1e9, 
  //setting 1e10 to be safe
  constexpr double neg_binomial_2_phi_cutoff = 1e10;
}

// NegBinomial(n|mu, phi)  [mu >= 0; phi > 0;  n >= 0]
template <bool propto, typename T_n, typename T_location, typename T_precision>
return_type_t<T_location, T_precision> neg_binomial_2_lpmf(
    const T_n& n, const T_location& mu, const T_precision& phi) {
  using T_partials_return = partials_return_t<T_n, T_location, T_precision>;

  static const char* function = "neg_binomial_2_lpmf";

  if (size_zero(n, mu, phi)) {
    return 0.0;
  }

  T_partials_return logp(0.0);
  check_nonnegative(function, "Failures variable", n);
  check_positive_finite(function, "Location parameter", mu);
  check_positive_finite(function, "Precision parameter", phi);
  check_consistent_sizes(function, "Failures variable", n, "Location parameter",
                         mu, "Precision parameter", phi);

  if (!include_summand<propto, T_location, T_precision>::value) {
    return 0.0;
  }

  using std::log;

  scalar_seq_view<T_n> n_vec(n);
  scalar_seq_view<T_location> mu_vec(mu);
  scalar_seq_view<T_precision> phi_vec(phi);
  size_t size = max_size(n, mu, phi);

  operands_and_partials<T_location, T_precision> ops_partials(mu, phi);

  size_t len_ep = max_size(mu, phi);
  size_t len_np = max_size(n, phi);

  VectorBuilder<true, T_partials_return, T_location> mu__(length(mu));
  for (size_t i = 0, size = length(mu); i < size; ++i) {
    mu__[i] = value_of(mu_vec[i]);
  }

  VectorBuilder<true, T_partials_return, T_precision> phi__(length(phi));
  for (size_t i = 0, size = length(phi); i < size; ++i) {
    phi__[i] = value_of(phi_vec[i]);
  }

  VectorBuilder<true, T_partials_return, T_precision> log_phi(length(phi));
  for (size_t i = 0, size = length(phi); i < size; ++i) {
    log_phi[i] = log(phi__[i]);
  }

  VectorBuilder<true, T_partials_return, T_location, T_precision>
      log_mu_plus_phi(len_ep);
  for (size_t i = 0; i < len_ep; ++i) {
    log_mu_plus_phi[i] = log(mu__[i] + phi__[i]);
  }

  VectorBuilder<true, T_partials_return, T_n, T_precision> n_plus_phi(len_np);
  for (size_t i = 0; i < len_np; ++i) {
    n_plus_phi[i] = n_vec[i] + phi__[i];
  }

  for (size_t i = 0; i < size; i++) {
    if(phi__[i] > internal::neg_binomial_2_phi_cutoff) {
      //Phi is large, deferring to Poisson. 
      //Copying the code here as just calling
      //poisson_lpmf does not preserve propto logic correctly
      if (include_summand<propto>::value) {
        logp -= lgamma(n_vec[i] + 1.0);
      }
      if (include_summand<propto, T_location>::value) {
        logp += multiply_log(n_vec[i], mu__[i]) - mu__[i];
      }

      // if (include_summand<propto, T_location, T_precision>::value) {
      //   logp += (mu__[i] * (mu__[i] - 2 * n_vec[i]) +
      //     n_vec[i] * (n_vec[i] - 1)) / ( 2 * phi__[i] ); 
      //   // logp += (mu__[i] * mu__[i] - n_vec[i] - 
      //   //   2 * mu__[i] * n_vec[i] + n_vec[i] * n_vec[i])/phi
      // }

      if (!is_constant_all<T_location>::value) {
        //This is the Taylor series of the full derivative for phi -> Inf
        //Obtained in Mathematica via 
        //Series[n/mu - (n + phi)/(mu+phi),{phi,Infinity, 1}]
        ops_partials.edge1_.partials_[i]
            += n_vec[i] / mu__[i]  + (mu__[i] - n_vec[i]) / phi__[i] - 1;              
      }
      
      //The derivative wrt. phi = 0 + O(1/neg_binomial_2_phi_cutoff^2)      
      //So ignoring here
      //Obtained in Mathematica via
      //Series[1 - (n + phi) / (mu + phi) + Log[phi] - Log[mu + phi] - 
      //  PolyGamma[phi] + PolyGamma[n + phi],{phi,Infinity, 1}]

    } else {
      if (include_summand<propto, T_precision>::value) {
        logp += binomial_coefficient_log(n_plus_phi[i] - 1, n_vec[i]);
      }
      if (include_summand<propto, T_precision>::value) {
        logp += multiply_log(phi__[i], phi__[i]);
      }
      if (include_summand<propto, T_location, T_precision>::value) {
        logp -= (n_plus_phi[i]) * log_mu_plus_phi[i];
      }
      if (include_summand<propto, T_location>::value) {
        logp += multiply_log(n_vec[i], mu__[i]);
      }

      if (!is_constant_all<T_location>::value) {
        ops_partials.edge1_.partials_[i]
            += n_vec[i] / mu__[i] - (n_vec[i] + phi__[i]) / (mu__[i] + phi__[i]);
      }
      if (!is_constant_all<T_precision>::value) {
        ops_partials.edge2_.partials_[i]
            += 1.0 - n_plus_phi[i] / (mu__[i] + phi__[i]) + log_phi[i]
              - log_mu_plus_phi[i] - digamma(phi__[i]) + digamma(n_plus_phi[i]);
      }    
    }
  }
  return ops_partials.build(logp);
}

template <typename T_n, typename T_location, typename T_precision>
inline return_type_t<T_location, T_precision> neg_binomial_2_lpmf(
    const T_n& n, const T_location& mu, const T_precision& phi) {
  return neg_binomial_2_lpmf<false>(n, mu, phi);
}

}  // namespace math
}  // namespace stan
#endif
