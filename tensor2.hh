#pragma once
#include <vector>
#include <assert.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <iostream>
#include <unordered_set>
#include <Eigen/Dense>

// goal, a tensor that does its own gradients

enum class TMode : uint8_t
{
  Unassigned = 0,
  Parameter=1,
  Addition=2,
  Mult=3,
  Div=4,
  Func=5,
  Max=6,
  Sum=7,
  Slice=8,
  Flatten = 9,
  DotProd= 10,
  LogSoftMax=11,
  Neg=12,
  Convo=13,
  Max2D=14
};

struct ReluFunc
{
  static float func(float f)
  {
    return std::max(0.0F, f);
  }
  static float deriv(float f)
  {
    return f < 0.0F ? 0.0F : 1.0F;
  }
};

struct TanhFunc
{
  static float func(float f)
  {
    return tanhf(f);
  }
  static float deriv(float f)
  {
    float t = tanhf(f);
    return 1-t*t;
  }
};

struct SigmoidFunc
{
  static float func(float in)
  {
    return 1.0F / (1.0F + expf(-in));
  }
  static float deriv(float in)
  {
    float sigma = 1.0F / (1.0F + expf(-in));
    return sigma * (1.0F - sigma);
  }
};


template<typename T=float>
struct TensorImp
{
  typedef TensorImp<T> us_t;
  TensorImp() : d_mode(TMode::Unassigned)
  {}
  
  TensorImp(unsigned int rows, unsigned int cols) :  d_mode(TMode::Parameter)
  {
    d_val = Eigen::MatrixX<T>(rows, cols);
    d_grads = Eigen::MatrixX<T>(rows, cols);
    d_accumgrads = Eigen::MatrixX<T>(rows, cols);
    d_grads.setZero();
    d_val.setZero();
    d_accumgrads.setZero();
    d_haveval = true;
  }

  TensorImp(std::shared_ptr<us_t> lhs, std::shared_ptr<us_t> rhs, TMode m) : d_lhs(lhs), d_rhs(rhs), d_mode(m)
  {
  }
  
  // we can have an embedded value, or one we have to calculate

  void assureValue() const
  {
    if(d_haveval || d_mode == TMode::Parameter)
      return;

    if(d_mode == TMode::Sum) {
      d_lhs->assureValue();
      Eigen::Matrix<float, 1, 1> v;
      v(0,0)= d_lhs->d_val.sum();
      d_val = v;
    }
    else if(d_mode == TMode::Addition) {
      d_lhs->assureValue();
      d_rhs->assureValue();
      d_val = d_lhs->d_val + d_rhs->d_val;
    }
    else if(d_mode == TMode::Mult) {
      d_lhs->assureValue();
      d_rhs->assureValue();
      d_val = d_lhs->d_val * d_rhs->d_val;
    }
    else if(d_mode == TMode::Neg) {
      d_lhs->assureValue();
      d_val = -d_lhs->d_val;
    }
    else if(d_mode == TMode::Div) {
      d_lhs->assureValue();
      d_rhs->assureValue();
      // so matrix division is "not really a thing"
      // we do support the special case where the RHS is a single number
      if(d_rhs->d_val.cols() != 1 || d_rhs->d_val.rows()!= 1)
        abort();
      d_val = d_lhs->d_val.array() / d_rhs->d_val(0,0);
    }
    else if(d_mode == TMode::DotProd) {
      d_lhs->assureValue();
      d_rhs->assureValue();
      d_val = d_lhs->d_val.cwiseProduct(d_rhs->d_val);
    }
    else if(d_mode == TMode::Slice) {
      d_lhs->assureValue();
      d_val = d_lhs->d_val.block(d_slicep.r, d_slicep.c, d_slicep.h, d_slicep.w);
    }
    else if(d_mode == TMode::Flatten) {
      d_lhs->assureValue();
      d_val = d_lhs->d_val.reshaped();
    }
    else if(d_mode == TMode::Func) {
      d_lhs->assureValue();
      d_val = d_lhs->d_val.unaryExpr(d_func);
    }
    else if(d_mode == TMode::LogSoftMax) {
      d_lhs->assureValue();
      auto lemax = d_lhs->d_val.maxCoeff();
      float sum =  (d_lhs->d_val.array() - lemax).exp().sum();
      d_val.array() = d_lhs->d_val.array() - lemax - log(sum);
    }
    else if(d_mode == TMode::Convo) {
      d_lhs->assureValue();
      d_rhs->assureValue(); // the weights

      d_val = Eigen::MatrixX<T>(1 + d_lhs->d_val.rows() - d_convop.kernel, 1 + d_lhs->d_val.cols() - d_convop.kernel);
      for(int r = 0 ; r < d_val.rows(); ++r)
        for(int c = 0 ; c < d_val.cols(); ++c)
          d_val(r,c) = d_lhs->d_val.block(r, c, d_convop.kernel, d_convop.kernel).cwiseProduct(d_rhs->d_val).sum()
            + d_convop.bias->d_val(0,0);
    }
    else if(d_mode == TMode::Max2D) {
      d_lhs->assureValue();
      // DOES NOT DO ANY PADDING!!
      assert((d_lhs->d_val.rows() % d_max2dp.kernel)==0 && (d_lhs->d_val.cols() % d_max2dp.kernel)==0); 
      d_val = Eigen::MatrixX<T>(d_lhs->d_val.rows()/d_max2dp.kernel, d_lhs->d_val.cols()/d_max2dp.kernel);
      for(int r = 0 ; r < d_lhs->d_val.rows(); r += d_max2dp.kernel)
        for(int c = 0 ; c < d_lhs->d_val.cols(); c += d_max2dp.kernel) {
          std::cout<<r<<","<<c<<","<<d_max2dp.kernel<<"\n";
          std::cout<<"Part\n"<<d_lhs->d_val.block(r, c, d_max2dp.kernel, d_max2dp.kernel)<<"\n";
          d_val(r/d_max2dp.kernel,c/d_max2dp.kernel) = d_lhs->d_val.block(r, c, d_max2dp.kernel, d_max2dp.kernel).maxCoeff();
        }
    }
    else {
      std::cerr<<"Unknown mode "<<(int)d_mode<< std::endl;
      abort();
    }
    
    d_grads = d_val; // silly way to get the dimensions right
    d_accumgrads = d_grads; // XXX must be a smarter way of doing this
    d_grads.setZero(); 
    d_accumgrads.setZero(); 
    d_haveval = true;
  }
  
  // these operate on 
  T& operator()(int row, int col)
  {
    assureValue();
    return d_val(row, col);
  }
  
  const T& operator()(int row, int col) const
  {
    assureValue();
    return d_val(row, col);
  }

  auto operator+(const TensorImp<T>& rhs) const
  {
    assureValue();
    rhs.assureValue();
    us_t ret;
    ret.d_val = d_val + rhs.d_val;
    return ret;
  }

  
  auto operator*(const TensorImp<T>& rhs) const
  {
    assureValue();
    rhs.assureValue();
    us_t ret;
    ret.d_val = (d_val * rhs.d_val);
    return ret;
  }

  // this function is key to the magic
  void build_topo(std::unordered_set<TensorImp<T>*>& visited, std::vector<TensorImp<T>*>& topo)
  {
    if(visited.count(this))
      return;
    visited.insert(this);
    
    if(d_lhs) {
      d_lhs->build_topo(visited, topo);
    }
    if(d_rhs) {
      d_rhs->build_topo(visited, topo);
    }
    topo.push_back(this);
  }

  void doGrad()
  {
    if(d_mode == TMode::Parameter) {
      return;
    }
    else if(d_mode == TMode::Flatten) {
      /*
      using namespace std;
      cout<<"flatten grad:\n";
      cout << "d_lhs->d_grads array\n"<< d_lhs->d_grads.array() << endl;
      cout << "d_grads array\n"<< d_grads.array() << endl;
      */
      d_lhs->d_grads.reshaped() += d_grads;
    }
    else if(d_mode == TMode::Addition) {
      d_lhs->d_grads += d_grads;
      d_rhs->d_grads += d_grads;
    }
    else if(d_mode == TMode::Neg) {
      d_lhs->d_grads -= d_grads;
    }
    else if(d_mode == TMode::Mult) {
      /*
      using namespace std;
      cout<<"d_val: \n"<<d_val <<endl;
      cout<<"d_grads: \n"<<d_grads<<endl;
      cout<<"rhs->d_val: \n"<<d_rhs->d_val << endl;
      cout<<"attempt lhs: \n"<<(d_grads * d_rhs->d_val.transpose())<<endl;
      */
      d_lhs->d_grads += (d_grads * d_rhs->d_val.transpose());
      /*
      cout<<"d_grads: \n"<<d_grads<<endl;
      cout<<"lhs->d_val: \n"<<d_lhs->d_val << endl;      
      cout<<"attempt rhs: \n"<<(d_lhs->d_val.transpose() * d_grads) << endl;
      */
      d_rhs->d_grads += (d_lhs->d_val.transpose() * d_grads);
    }
    else if(d_mode == TMode::Div) { // so matrix division is "not really a thing"
      d_lhs->d_grads.array() += (d_grads.array() / d_rhs->d_val(0,0));
      /// XXX super wrong I bet
      //d_rhs->d_grads +=(-d_grads * d_rhs->d_val / (d_rhs->d_val * d_rhs->d_val));
    }
    else if(d_mode == TMode::DotProd) {
      d_lhs->d_grads.array() += d_rhs->d_val.array();
      d_rhs->d_grads.array() += d_lhs->d_val.array();
    }
    else if(d_mode == TMode::Slice) {
      d_lhs->d_grads.block(d_slicep.r, d_slicep.c, d_slicep.h, d_slicep.w) += d_grads;
    }
    else if(d_mode == TMode::Sum) {
      using namespace std;
      d_lhs->d_grads.array() += d_lhs->d_grads.array() + d_grads(0,0);
    }
    else if(d_mode == TMode::LogSoftMax) {
      // I cargo culted this so it produces the same numbers as PyTorch
      d_lhs->d_grads.array() += d_grads.array() - d_val.array().exp()*d_grads.sum();
      // it looks like magic, but it really works: https://stackoverflow.com/questions/35304393/trying-to-understand-code-that-computes-the-gradient-wrt-to-the-input-for-logsof
      // https://github.com/torch/nn/blob/master/lib/THNN/generic/LogSoftMax.c
      // https://math.stackexchange.com/questions/2013050/log-of-softmax-function-derivative
    }
    else if(d_mode == TMode::Func) {
      using namespace std;
      /*
      cout<<"\nFUNC\n";
      cout<<"d_lhs->d_grads: "<<d_lhs->d_grads<<endl;
      cout<<"d_grads: "<<d_grads<<endl;

      cout<<"tmp:\n"<<d_lhs->d_val.unaryExpr(d_deriv)<<"\n";
      */
      d_lhs->d_grads.array() += d_grads.array()*d_lhs->d_val.unaryExpr(d_deriv).array();
        //        d_lhs->d_grads.array() + d_grads(0,0);
      /*
      cout<<"d_lhs->d_grads after: "<<d_lhs->d_grads<<endl;
      cout<<"\n";
      */
    }
    else if(d_mode == TMode::Convo) {
      // weights in d_rhs
      // need to convey grads to input (d_lhs), weights (r_hs) and bias
      // if kernel is same size, convolution delivers a single number in d_val
      // the grad of the kernel is then the values of the input and vv

      // like this:
      //      d_lhs->d_grads.array() += d_rhs->d_val.array();
      //      d_rhs->d_grads.array() += d_lhs->d_val.array();
      //
      // if the kernel is smaller, it needs to walk over the input and add up
      // itself to the grads there, and conversely, add up the grads from there to itself (??)


      // the output (d_val) has shape 1 + d_lhs (input)  - d_rhs (filters)
      for(int r = 0 ; r < d_val.rows(); ++r)
        for(int c = 0 ; c < d_val.cols(); ++c)
          d_lhs->d_grads.block(r,c,d_convop.kernel, d_convop.kernel)  += d_rhs->d_val;

      // now add grads to the filter - note that this convolves over blocks
      // the size of the output!
      for(int r = 0 ; r < d_rhs->d_val.rows(); ++r)
        for(int c = 0 ; c < d_rhs->d_val.cols(); ++c)
          d_rhs->d_grads(r,c) += d_lhs->d_val.block(r, c, d_val.rows(), d_val.cols()).sum();

      d_convop.bias->d_grads(0,0) += d_val.rows() * d_val.cols(); // we zero this explicitly in get grads
    }
    else if(d_mode == TMode::Max2D) {
      for(int r = 0 ; r < d_lhs->d_val.rows(); r += d_max2dp.kernel) {
        for(int c = 0 ; c < d_lhs->d_val.cols(); c += d_max2dp.kernel) {
          unsigned int mrow=0, mcol=0;
          d_lhs->d_val.block(r, c, d_max2dp.kernel, d_max2dp.kernel).maxCoeff(&mrow, &mcol);
          d_lhs->d_grads(r+mrow, c+mcol)++;
        }
      }
    }
    else abort();
  }
  
  mutable Eigen::MatrixX<T> d_val, d_grads, d_accumgrads;
  std::function<T(T)> d_func, d_deriv;
  std::shared_ptr<us_t> d_lhs, d_rhs;
  TMode d_mode;
  mutable bool d_haveval = false;
  struct SliceParams
  {
    int r, c;
    int h, w;
  } d_slicep;

  struct Max2DParams
  {
    int kernel;
  } d_max2dp;

  struct ConvoParams
  {
    int kernel;
    std::shared_ptr<TensorImp<T>> bias;
  } d_convop;

};


template<typename T=float>
struct Tensor
{
  typedef Tensor<T> us_t;
  Tensor() : d_imp(std::make_shared<TensorImp<T>>())
  {}

  Tensor(unsigned int rows, unsigned int cols) : d_imp(std::make_shared<TensorImp<T>>(rows, cols))
  {}

  T& operator()(int x, int y)
  {
    return (*d_imp)(x, y);
  }
  
  const T& operator()(int x, int y) const
  {
    return (*d_imp)(x, y);
  }

  Tensor<T> sum()
  {
    Tensor<T> ret;
    ret.d_imp = std::make_shared<TensorImp<T>>(d_imp, std::shared_ptr<TensorImp<T>>(), TMode::Sum);
    return ret;
  }

  std::vector<TensorImp<T>* > getTopo()
  {
    std::vector<TensorImp<T>* > topo;
    std::unordered_set<TensorImp<T>* > visited;
    d_imp->build_topo(visited, topo);
    topo.shrink_to_fit();
    return topo;
  }

  
  void backward(std::vector<TensorImp<T>*> topo=0)
  {
    d_imp->assureValue();
    d_imp->d_grads = d_imp->d_val; // dimensions
    d_imp->d_grads.setConstant(1.0);
    for(auto iter = topo.rbegin(); iter != topo.rend(); ++iter) {
      (*iter)->doGrad();
    }
  }

  void zerograd(std::vector<TensorImp<T>*> topo=0)
  {
    for(auto iter = topo.rbegin(); iter != topo.rend(); ++iter) { 
      (*iter)->d_grads.setConstant(0);
      if((*iter)->d_mode != TMode::Parameter)
        (*iter)->d_haveval = false;
      if((*iter)->d_mode == TMode::Convo) // UGLY
        (*iter)->d_convop.bias->d_grads(0,0)=0;
    }
  }

  Eigen::MatrixX<T> getGrad()
  {
    return d_imp->d_grads;
  }

  Eigen::MatrixX<T> getAccumGrad()
  {
    return d_imp->d_accumgrads;
  }

  void zeroAccumGrads(std::vector<TensorImp<T>*> topo)
  {
    for(auto iter = topo.rbegin(); iter != topo.rend(); ++iter) { 
      (*iter)->d_accumgrads = Eigen::MatrixX<T>::Constant((*iter)->d_grads.rows(), (*iter)->d_grads.cols(), 0.0F);
    }
  }
  
  void accumGrads(std::vector<TensorImp<T>*> topo)
  {
    for(auto iter = topo.rbegin(); iter != topo.rend(); ++iter) { 
      (*iter)->d_accumgrads += (*iter)->d_grads;
    }
  }

    
  
  void randomize(float fact)
  {
    d_imp->d_mode = TMode::Parameter;
    d_imp->d_val = Eigen::MatrixX<T>::Random(d_imp->d_val.rows(), d_imp->d_val.cols()); // XXX probably not right
    d_imp->d_val.array() *= fact;
  }

  void zero()
  {
    d_imp->d_mode = TMode::Parameter;
    d_imp->d_val = Eigen::MatrixX<T>::Zero(d_imp->d_val.rows(), d_imp->d_val.cols()); 
  }
  void constant(float f)
  {
    d_imp->d_mode = TMode::Parameter;
    d_imp->d_val = Eigen::MatrixX<T>::Constant(d_imp->d_val.rows(), d_imp->d_val.cols(), f); 
  }
  void iota(float f)
  {
    d_imp->d_mode = TMode::Parameter;
    for(unsigned int r = 0 ; r < d_imp->d_val.rows(); ++r) {
      for(unsigned int c = 0 ; c < d_imp->d_val.cols(); ++c) {
        d_imp->d_val(r,c)= f++;
      }
    }
  }

  auto& operator-=(const Eigen::MatrixX<T>& rhs)
  {
    d_imp->d_val -= rhs;
    return *this;
  }

  unsigned int maxValueIndexOfColumn(int c)
  {
    Eigen::Index maxRow, maxCol;
    d_imp->d_val.maxCoeff(&maxRow, &maxCol);
    return maxRow;
  }

  Tensor<T> dot(const Tensor<T>& rhs)
  {
    Tensor<T> ret;
    ret.d_imp = std::make_shared<TensorImp<T>>(d_imp, rhs.d_imp, TMode::DotProd);
    return ret;
  }

  Tensor<T> makeSlice(int r, int c, int h, int w=-1)
  {
    if(w <= 0)
      w = h;
    Tensor<T> ret;
    ret.d_imp = std::make_shared<TensorImp<T>>(d_imp, std::shared_ptr<TensorImp<T>>(), TMode::Slice);
    ret.d_imp->d_slicep={r, c, h, w};
    return ret;
  }

  Tensor<T> makeConvo(int kernel, Tensor<T>& weights, Tensor<T>& bias)
  {
    Tensor<T> ret;
    ret.d_imp = std::make_shared<TensorImp<T>>(d_imp, weights.d_imp, TMode::Convo);
    ret.d_imp->d_convop.kernel = kernel;
    ret.d_imp->d_convop.bias = bias.d_imp;
    return ret;
  }

  Tensor<T> makeMax2d(int kernel)
  {
    Tensor<T> ret;
    ret.d_imp = std::make_shared<TensorImp<T>>(d_imp, std::shared_ptr<TensorImp<T>>(), TMode::Max2D);
    ret.d_imp->d_max2dp.kernel = kernel;
    return ret;
  }

  
  
  Tensor<T> makeFlatten()
  {
    Tensor<T> ret;
    ret.d_imp = std::make_shared<TensorImp<T>>(d_imp, std::shared_ptr<TensorImp<T>>(), TMode::Flatten);
    return ret;
  }

  unsigned int getRows() const
  {
    return d_imp->d_val.rows();
  }
  unsigned int getCols() const
  {
    return d_imp->d_val.cols();
  }

  std::shared_ptr<TensorImp<T>> d_imp;
};

template<typename T>
inline Tensor<T> operator+(const Tensor<T>& lhs, const Tensor<T>& rhs)
{
  Tensor<T> ret;
  ret.d_imp = std::make_shared<TensorImp<T>>(lhs.d_imp, rhs.d_imp, TMode::Addition);
  return ret;
}

template<typename T>
Tensor<T> operator-(const Tensor<T>& lhs, const Tensor<T>& rhs)
{
  Tensor<T> neg;
  neg.d_imp = std::make_shared<TensorImp<T>>(rhs.d_imp, std::shared_ptr<TensorImp<T>>(), TMode::Neg);

  return lhs + neg;
}


template<typename T>
inline Tensor<T> operator-(const Tensor<T>& lhs) 
{
  Tensor<T> neg;
  neg.d_imp = std::make_shared<TensorImp<T>>(lhs.d_imp, std::shared_ptr<TensorImp<T>>(), TMode::Neg);
  return neg;
}


template<typename T>
inline Tensor<T> operator*(const Tensor<T>& lhs, const Tensor<T>& rhs)
{
  Tensor<T> ret;
  ret.d_imp = std::make_shared<TensorImp<T>>(lhs.d_imp, rhs.d_imp, TMode::Mult);
  return ret;
}

template<typename T>
inline Tensor<T> operator/(const Tensor<T>& lhs, const Tensor<T>& rhs)
{
  Tensor<T> ret;
  assert(rhs.d_imp->d_val.cols() == 1 && rhs.d_imp->d_val.rows() == 1);
  ret.d_imp = std::make_shared<TensorImp<T>>(lhs.d_imp, rhs.d_imp, TMode::Div);
  return ret;
}


template<typename F, typename T>
inline Tensor<T> makeFunction(const Tensor<T>& lhs)
{
  Tensor<T> ret;
  ret.d_imp = std::make_shared<TensorImp<T>>(lhs.d_imp, std::shared_ptr<TensorImp<T>>(), TMode::Func);
  ret.d_imp->d_func = &F::func;
  ret.d_imp->d_deriv = &F::deriv;
  return ret;
}
template<typename T>
inline Tensor<T> makeLogSoftMax(const Tensor<T>& lhs)
{
  Tensor<T> ret;
  ret.d_imp = std::make_shared<TensorImp<T>>(lhs.d_imp, std::shared_ptr<TensorImp<T>>(), TMode::LogSoftMax);
  return ret;
}

template<typename T>
std::ostream& operator<<(std::ostream& os, const Tensor<T>& ns)
{
  ns.d_imp->assureValue();
  os <<ns.d_imp->d_val;
  return os;
}

template<typename T>
void printImgTensor(const T& img)
{
  for(unsigned int y=0; y < img.getRows(); ++y) {
    for(unsigned int x=0; x < img.getCols(); ++x) {
      float val = img(y,x);
      if(val > 0.5)
        std::cout<<'X';
      else if(val > 0.25)
        std::cout<<'*';
      else if(val > 0.125)
        std::cout<<'.';
      else
        std::cout<<' ';
    }
    std::cout<<'\n';
  }
  std::cout<<"\n";
}
