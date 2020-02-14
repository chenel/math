#ifndef STAN_MATH_OPENCL_KERNEL_GENERATOR_OPERATION_HPP
#define STAN_MATH_OPENCL_KERNEL_GENERATOR_OPERATION_HPP
#ifdef STAN_OPENCL

#include <stan/math/prim/meta.hpp>
#include <stan/math/opencl/kernel_generator/type_str.hpp>
#include <stan/math/opencl/kernel_generator/name_generator.hpp>
#include <stan/math/opencl/kernel_generator/is_valid_expression.hpp>
#include <stan/math/opencl/matrix_cl_view.hpp>
#include <stan/math/opencl/matrix_cl.hpp>
#include <stan/math/opencl/kernel_cl.hpp>
#include <CL/cl2.hpp>
#include <algorithm>
#include <string>
#include <tuple>
#include <set>
#include <array>
#include <numeric>

namespace stan {
namespace math {

/**
 * Parts of an OpenCL kernel, generated by an expression
 */
struct kernel_parts {
  std::string initialization;  // the code for initializations done by all
                               // threads, even if they have no work
  std::string body_prefix;     // the code that should be placed at the start of
                               // the kernel body
  std::string body;       // the body of the kernel - code executing operations
  std::string reduction;  // the code for reductions within work group by all
                          // threads, even if they have no work
  std::string args;       // kernel arguments
};

/**
 * Base for all kernel generator operations.
 * @tparam Derived derived type
 * @tparam Scalar scalar type of the result
 * @tparam Args types of arguments to this operation
 */
template <typename Derived, typename Scalar, typename... Args>
class operation_cl : public operation_cl_base {
  static_assert(
      conjunction<std::is_base_of<operation_cl_base,
                                  std::remove_reference_t<Args>>...>::value,
      "operation_cl: all arguments to operation must be operations!");

 protected:
  std::tuple<Args...> arguments_;
  mutable std::string var_name;  // name of the variable that holds result of
                                 // this operation in the kernel

  /**
   * Casts the instance into its derived type.
   * @return \c this cast into derived type
   */
  inline Derived& derived() { return *static_cast<Derived*>(this); }

  /**
   * Casts the instance into its derived type.
   * @return \c this cast into derived type
   */
  inline const Derived& derived() const {
    return *static_cast<const Derived*>(this);
  }

 public:
  using Deriv = Derived;
  static const bool require_specific_local_size;
  // number of arguments this operation has
  static constexpr int N = sizeof...(Args);
  // value representing a not yet determined size
  static const int dynamic = -1;

  /**
   * Constructor
   * @param arguments Arguments of this expression that are also valid
   * expressions
   */
  explicit operation_cl(Args&&... arguments)
      : arguments_(std::forward<Args>(arguments)...) {}

  /**
   * Evaluates the expression.
   * @return Result of the expression.
   */
  matrix_cl<Scalar> eval() const {
    matrix_cl<Scalar> res(derived().rows(), derived().cols(), derived().view());
    if (res.size() > 0) {
      this->evaluate_into(res);
    }
    return res;
  }

  /**
   * Converting to \c matrix_cl evaluates the expression. Used when assigning to
   * a \c matrix_cl.
   */
  operator matrix_cl<Scalar>() const { return derived().eval(); }

  /**
   * Evaluates \c this expression into given left-hand-side expression.
   * If the kernel for this expression is not cached it is generated and then
   * executed.
   * @tparam T_lhs type of the left-hand-side expression
   * @param lhs Left-hand-side expression
   */
  template <typename T_lhs>
  inline void evaluate_into(const T_lhs& lhs) const;

  /**
   * Generates kernel source for evaluating \c this expression into given
   * left-hand-side expression.
   * @tparam T_lhs type of the left-hand-side expression
   * @param lhs Left-hand-side expression
   * @return kernel source
   */
  template <typename T_lhs>
  inline std::string get_kernel_source_for_evaluating_into(
      const T_lhs& lhs) const;

  template <typename T_lhs>
  struct cache {
    static std::string source;  // kernel source - not used anywhere. Only
                                // intended for debugging.
    static cl::Kernel kernel;   // cached kernel - different for every
                                // combination of template instantination of \c
                                // operation and every \c T_lhs
  };

  /**
   * Generates kernel code for assigning this expression into result expression.
   * @param[in,out] generated set of (pointer to) already generated operations
   * @param name_gen name generator for this kernel
   * @param i row index variable name
   * @param j column index variable name
   * @param result expression into which result is to be assigned
   * @return part of kernel with code for this and nested expressions
   */
  template <typename T_result>
  kernel_parts get_whole_kernel_parts(
      std::set<const operation_cl_base*>& generated, name_generator& ng,
      const std::string& i, const std::string& j, const T_result& result) const {
    kernel_parts parts = derived().get_kernel_parts(generated, ng, i, j);
    kernel_parts out_parts = result.get_kernel_parts_lhs(generated, ng, i, j);

    parts.args += out_parts.args;
    parts.body += out_parts.body + " = " + derived().var_name + ";\n";
    return parts;
  }

  /**
   * generates kernel code for this and nested expressions.
   * @param[in,out] generated set of (pointer to) already generated operations
   * @param name_gen name generator for this kernel
   * @param i row index variable name
   * @param j column index variable name
   * @return part of kernel with code for this and nested expressions
   */
  inline kernel_parts get_kernel_parts(
      std::set<const operation_cl_base*>& generated, name_generator& name_gen,
      const std::string& i, const std::string& j) const {
    kernel_parts res{};
    if (generated.count(this) == 0) {
      this->var_name = name_gen.generate();
      generated.insert(this);
      std::string i_arg = i;
      std::string j_arg = j;
      derived().modify_argument_indices(i_arg, j_arg);
      std::array<kernel_parts, N> args_parts = index_apply<N>([&](auto... Is) {
        return std::array<kernel_parts, N>{
            std::get<Is>(arguments_)
                .get_kernel_parts(generated, name_gen, i_arg, j_arg)...};
      });
      res.initialization
          = std::accumulate(args_parts.begin(), args_parts.end(), std::string(),
                            [](const std::string& a, const kernel_parts& b) {
                              return a + b.initialization;
                            });
      res.body
          = std::accumulate(args_parts.begin(), args_parts.end(), std::string(),
                            [](const std::string& a, const kernel_parts& b) {
                              return a + b.body;
                            });
      res.reduction
          = std::accumulate(args_parts.begin(), args_parts.end(), std::string(),
                            [](const std::string& a, const kernel_parts& b) {
                              return a + b.reduction;
                            });
      res.args
          = std::accumulate(args_parts.begin(), args_parts.end(), std::string(),
                            [](const std::string& a, const kernel_parts& b) {
                              return a + b.args;
                            });
      kernel_parts my_part = index_apply<N>([&](auto... Is) {
        return this->derived().generate(i, j,
                                        std::get<Is>(arguments_).var_name...);
      });
      res.initialization += my_part.initialization;
      res.body = my_part.body_prefix + res.body + my_part.body;
      res.args += my_part.args;
      res.reduction += my_part.reduction;
    }
    return res;
  }

  /**
   * Does nothing. Derived classes can override this to modify how indices are
   * passed to its argument expressions. On input arguments \c i and \c j are
   * expressions for indices of this operation. On output they are expressions
   * for indices of argument operations.
   * @param[in, out] i row index
   * @param[in, out] j column index
   */
  inline void modify_argument_indices(std::string& i, std::string& j) const {}

  /**
   * Sets kernel arguments for nested expressions.
   * @param[in,out] generated set of expressions that already set their kernel
   * arguments
   * @param kernel kernel to set arguments on
   * @param[in,out] arg_num consecutive number of the first argument to set.
   * This is incremented for each argument set by this function.
   */
  inline void set_args(std::set<const operation_cl_base*>& generated,
                       cl::Kernel& kernel, int& arg_num) const {
    if (generated.count(this) == 0) {
      generated.insert(this);
      // parameter pack expansion returns a comma-separated list of values,
      // which can not be used as an expression. We work around that by using
      // comma operator to get a list of ints, which we use to construct an
      // initializer_list from. Cast to voids avoids warnings about unused
      // expression.
      index_apply<N>([&](auto... Is) {
        static_cast<void>(std::initializer_list<int>{
            (std::get<Is>(arguments_).set_args(generated, kernel, arg_num),
             0)...});
      });
    }
  }

  /**
   * Adds read event to any matrices used by nested expressions.
   * @param e the event to add
   */
  inline void add_read_event(cl::Event& e) const {
    index_apply<N>([&](auto... Is) {
      (void)std::initializer_list<int>{
          (std::get<Is>(arguments_).add_read_event(e), 0)...};
    });
  }

  /**
   * Number of rows of a matrix that would be the result of evaluating this
   * expression. Some subclasses may need to override this.
   * @return number of rows
   */
  inline int rows() const {
    return index_apply<N>([&](auto... Is) {
      // assuming all non-dynamic sizes match
      return std::max({std::get<Is>(arguments_).rows()...});
    });
  }

  /**
   * Number of columns of a matrix that would be the result of evaluating this
   * expression. Some subclasses may need to override this.
   * @return number of columns
   */
  inline int cols() const {
    return index_apply<N>([&](auto... Is) {
      // assuming all non-dynamic sizes match
      return std::max({std::get<Is>(arguments_).cols()...});
    });
  }

  /**
   * Number of rows threads need to be launched for. For most expressions this
   * equals number of rows of the result.
   * @return number of rows
   */
  inline int thread_rows() const { return derived().rows(); }

  /**
   * Number of columns threads need to be launched for. For most expressions
   * this equals number of cols of the result.
   * @return number of columns
   */
  inline int thread_cols() const { return derived().cols(); }

  /**
   * Determine index of bottom diagonal written. Some subclasses may need to
   * override this.
   * @return number of columns
   */
  inline int bottom_diagonal() const {
    return index_apply<N>([&](auto... Is) {
      return std::min({std::get<Is>(arguments_).bottom_diagonal()...});
    });
  }

  /**
   * Determine index of top diagonal written. Some subclasses may need to
   * override this.
   * @return number of columns
   */
  inline int top_diagonal() const {
    return index_apply<N>([&](auto... Is) {
      return std::max({std::get<Is>(arguments_).top_diagonal()...});
    });
  }
};

template <typename Derived, typename Scalar, typename... Args>
template <typename T_lhs>
cl::Kernel operation_cl<Derived, Scalar, Args...>::cache<T_lhs>::kernel;

template <typename Derived, typename Scalar, typename... Args>
template <typename T_lhs>
std::string operation_cl<Derived, Scalar, Args...>::cache<T_lhs>::source;

template <typename Derived, typename Scalar, typename... Args>
const bool operation_cl<Derived, Scalar, Args...>::require_specific_local_size =
    std::max({false, std::decay_t<Args>::Deriv::require_specific_local_size...});
}  // namespace math
}  // namespace stan

#endif
#endif
