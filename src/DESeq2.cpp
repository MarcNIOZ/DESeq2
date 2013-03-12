
// includes from the plugin
#include <RcppArmadillo.h>
#include <Rcpp.h>


#ifndef BEGIN_RCPP
#define BEGIN_RCPP
#endif

#ifndef END_RCPP
#define END_RCPP
#endif

using namespace Rcpp;


// user includes
#include <R.h>
#include <Rmath.h>
#include <stdio.h>
#include <R_ext/Utils.h>

// this function returns the log likelihood of dispersion parameter alpha, for negative binomial variables
// given the counts y, the expected means mu, the design matrix x (used for calculating the Cox-Reid adjustment),
// and the parameters for the normal prior on log alpha
double ll_prior_cr(double log_alpha, Rcpp::NumericMatrix::Row y, Rcpp::NumericMatrix::Row mu, arma::mat x, double log_alpha_prior_mean, double log_alpha_prior_sigmasq, bool use_prior) {
  double prior_part;
  double alpha = exp(log_alpha);
  Rcpp::NumericVector w_diag = pow(pow(mu, -1) + alpha, -1);
  arma::mat w = arma::diagmat(Rcpp::as<arma::vec>(w_diag));
  arma::mat b = x.t() * w * x;
  double cr_term = -0.5 * log(det(b));
  double alpha_neg1 = R_pow_di(alpha, -1);
  double ll_part = sum(lgamma(y + alpha_neg1) - Rf_lgammafn(alpha_neg1) - y * log(mu + alpha_neg1) - alpha_neg1 * log(1.0 + mu * alpha));
  if (use_prior) {
    prior_part = -0.5 * R_pow_di(log(alpha) - log_alpha_prior_mean,2)/log_alpha_prior_sigmasq;
  } else {
    prior_part = 0.0;
  }
  double res =  ll_part + prior_part + cr_term;
  return(res);
}

// this function returns the derivative of the log likelihood with respect to the log of the 
// dispersion parameter alpha, given the same inputs as the previous function
double dll_prior_cr(double log_alpha, Rcpp::NumericMatrix::Row y, Rcpp::NumericMatrix::Row mu, arma::mat x, double log_alpha_prior_mean, double log_alpha_prior_sigmasq, bool use_prior) {
  double prior_part;
  double alpha = exp(log_alpha);
  Rcpp::NumericVector w_diag = pow(pow(mu, -1) + alpha, -1);
  arma::mat w = arma::diagmat(Rcpp::as<arma::vec>(w_diag));
  Rcpp::NumericVector dw_diag = -1.0 * pow(pow(mu, -1) + alpha, -2);
  arma::mat dw = arma::diagmat(Rcpp::as<arma::vec>(dw_diag));
  arma::mat b = x.t() * w * x;
  arma::mat db = x.t() * dw * x;
  double ddetb = ( det(b) * trace(b.i() * db) );
  double cr_term = -0.5 * ddetb / det(b);
  double alpha_neg1 = R_pow_di(alpha, -1);
  double alpha_neg2 = R_pow_di(alpha, -2);
  double ll_part = alpha_neg2 * sum(Rf_digamma(alpha_neg1) + log(1 + mu*alpha) - mu*alpha*pow(1.0 + mu*alpha, -1) - digamma(y + alpha_neg1) + y * pow(mu + alpha_neg1, -1));
  if (use_prior) {
    prior_part = -1.0 * (log(alpha) - log_alpha_prior_mean)/log_alpha_prior_sigmasq;
  } else {
    prior_part = 0.0;
  }
  // Note: return alpha * derivative because we take derivatives w.r.t log alpha
  double res = (ll_part + cr_term) * alpha + prior_part;
  return(res);
}


// declarations
extern "C" {
SEXP fitDisp( SEXP ySEXP, SEXP xSEXP, SEXP mu_hatSEXP, SEXP log_alphaSEXP, SEXP log_alpha_prior_meanSEXP, SEXP log_alpha_prior_sigmasqSEXP, SEXP min_log_alphaSEXP, SEXP kappa_0SEXP, SEXP tolSEXP, SEXP maxitSEXP, SEXP use_priorSEXP) ;
SEXP fitBeta( SEXP ySEXP, SEXP xSEXP, SEXP nfSEXP, SEXP alpha_hatSEXP, SEXP beta_matSEXP, SEXP lambdaSEXP, SEXP tolSEXP, SEXP maxitSEXP, SEXP largeSEXP) ;
}

// definition

SEXP fitDisp( SEXP ySEXP, SEXP xSEXP, SEXP mu_hatSEXP, SEXP log_alphaSEXP, SEXP log_alpha_prior_meanSEXP, SEXP log_alpha_prior_sigmasqSEXP, SEXP min_log_alphaSEXP, SEXP kappa_0SEXP, SEXP tolSEXP, SEXP maxitSEXP, SEXP use_priorSEXP ){
BEGIN_RCPP
Rcpp::NumericMatrix y(ySEXP);
arma::mat x = Rcpp::as<arma::mat>(xSEXP);
int y_n = y.nrow();
Rcpp::NumericVector log_alpha(clone(log_alphaSEXP));
Rcpp::NumericMatrix mu_hat(mu_hatSEXP);
Rcpp::NumericVector log_alpha_prior_mean(log_alpha_prior_meanSEXP);
double log_alpha_prior_sigmasq = Rcpp::as<double>(log_alpha_prior_sigmasqSEXP);
double min_log_alpha = Rcpp::as<double>(min_log_alphaSEXP);
double kappa_0 = Rcpp::as<double>(kappa_0SEXP);
int maxit = Rcpp::as<int>(maxitSEXP);
double epsilon = 1.0e-4;
double a, a_propose, kappa, ll, llnew, dll, theta_kappa, theta_hat_kappa, change;
Rcpp::NumericVector initial_ll(y_n);
Rcpp::NumericVector initial_dll(y_n);
Rcpp::NumericVector last_ll(y_n);
Rcpp::NumericVector last_dll(y_n);
Rcpp::NumericVector last_change(y_n);
Rcpp::IntegerVector iter(y_n);
Rcpp::IntegerVector iter_accept(y_n);
 Rcpp::NumericMatrix kappa_matrix(y_n,maxit);
double tol = Rcpp::as<double>(tolSEXP);
bool use_prior = Rcpp::as<bool>(use_priorSEXP);

for (int i = 0; i < y_n; i++) {
  R_CheckUserInterrupt();
  Rcpp::NumericMatrix::Row yrow = y(i,_);
  Rcpp::NumericMatrix::Row mu_hat_row = mu_hat(i,_);
  // maximize the log likelihood over the variable a, the log of alpha, the dispersion parameter
  // in order to express the optimization in a typical manner, 
  // for calculating theta(kappa) we multiple the log likelihood by -1 and seek a minimum
  a = log_alpha(i);
  // we use a line search based on the Armijo rule
  // define a function theta(kappa) = f(a + kappa * d), where d is the search direction
  // in this case the search direction is taken by the first derivative of the log likelihood
  ll = ll_prior_cr(a, yrow, mu_hat_row, x, log_alpha_prior_mean(i), log_alpha_prior_sigmasq, use_prior);
  dll = dll_prior_cr(a, yrow, mu_hat_row, x, log_alpha_prior_mean(i), log_alpha_prior_sigmasq, use_prior);
  kappa = kappa_0;
  initial_ll(i) = ll;
  initial_dll(i) = dll;
  last_change(i) = -1.0;
  for (int t = 0; t < maxit; t++) {
    kappa_matrix(i,t) = kappa;
    // iter counts the number of steps taken out of  maxit;
    iter(i)++;
    a_propose = a + kappa * dll;
    // note: lgamma is unstable for values around 1e17, where there is a switch in lgamma.c
    // we limit log alpha from going lower than -30
    if (a_propose < -30.0) {
      kappa = (-30.0 - a)/dll;
    }
    // note: we limit log alpha from going higher than 10
    if (a_propose > 10.0) {
      kappa = (10.0 - a)/dll;
    }
    theta_kappa = -1.0 * ll_prior_cr(a + kappa*dll, yrow, mu_hat_row, x, log_alpha_prior_mean(i), log_alpha_prior_sigmasq, use_prior);
    theta_hat_kappa = -1.0 * ll - kappa * epsilon * R_pow_di(dll, 2);
    // if this inequality is true, we have satisfied the Armijo rule and 
    // accept the step size kappa, otherwise we halve kappa
    if (theta_kappa <= theta_hat_kappa) {
      // iter_accept counts the number of accepted proposals;
      iter_accept(i)++;
      a = a + kappa * dll;
      llnew = ll_prior_cr(a, yrow, mu_hat_row, x, log_alpha_prior_mean(i), log_alpha_prior_sigmasq, use_prior);
      // look for change in log likelihood
      change = llnew - ll;
      if (change < tol) {
	ll = llnew;
	break;
      }
      // if log(alpha) is going to -infinity
      // break the loop
      if (a < min_log_alpha) {
	break;
      }
      ll = llnew;
      dll = dll_prior_cr(a, yrow, mu_hat_row, x, log_alpha_prior_mean(i), log_alpha_prior_sigmasq, use_prior);
      // instead of resetting kappa to kappa_0 
      // multiple kappa by 1.1
      kappa = fmin(kappa * 1.1, kappa_0);
      // every 5 accepts, halve kappa
      // to prevent slow convergence
      // due to overshooting
      if (iter_accept(i) % 5 == 0) {
	kappa = kappa / 2.0;
      }
    } else {
      kappa = kappa / 2.0;
    }
  }
  last_ll(i) = ll;
  last_dll(i) = dll;
  log_alpha(i) = a;
  // last change indicates the change for the final iteration
  last_change(i) = change;
}

return Rcpp::List::create(Rcpp::Named("log_alpha",log_alpha),
			  Rcpp::Named("iter",iter),
			  Rcpp::Named("iter_accept",iter_accept),
			  Rcpp::Named("last_change",last_change),
			  Rcpp::Named("initial_dll",initial_dll),
			  Rcpp::Named("initial_ll",initial_ll),
			  Rcpp::Named("kappa_matrix",kappa_matrix),
			  Rcpp::Named("last_dll",last_dll),
			  Rcpp::Named("last_ll",last_ll));
END_RCPP
}


SEXP fitBeta( SEXP ySEXP, SEXP xSEXP, SEXP nfSEXP, SEXP alpha_hatSEXP, SEXP beta_matSEXP, SEXP lambdaSEXP, SEXP tolSEXP, SEXP maxitSEXP, SEXP largeSEXP ){
BEGIN_RCPP
arma::mat y = Rcpp::as<arma::mat>(ySEXP);
arma::mat nf = Rcpp::as<arma::mat>(nfSEXP);
arma::mat x = Rcpp::as<arma::mat>(xSEXP);
int y_n = y.n_rows;
arma::vec alpha_hat = Rcpp::as<arma::vec>(alpha_hatSEXP);
arma::mat beta_mat = Rcpp::as<arma::mat>(beta_matSEXP);
arma::mat beta_var_mat = arma::zeros(beta_mat.n_rows, beta_mat.n_cols);
arma::colvec lambda = Rcpp::as<arma::colvec>(lambdaSEXP);
int maxit = Rcpp::as<int>(maxitSEXP);
arma::colvec yrow, nfrow, beta_hat, mu_hat, z, beta_hat_new;
arma::mat w, ridge;
double tol = Rcpp::as<double>(tolSEXP);
double large = Rcpp::as<double>(largeSEXP);
Rcpp::NumericVector beta_hat_nv;
Rcpp::NumericVector beta_hat_new_nv;
Rcpp::LogicalVector too_large;
Rcpp::NumericVector change;
Rcpp::NumericMatrix last_change(beta_mat.n_rows, beta_mat.n_cols);
Rcpp::NumericVector iter(y_n);

for (int i = 0; i < y_n; i++) {
  R_CheckUserInterrupt();
  nfrow = nf.row(i).t();
  yrow = y.row(i).t();
  beta_hat = beta_mat.row(i).t();
  for (int t = 0; t < maxit; t++) {
    iter(i)++;
    mu_hat = nfrow % exp(x * beta_hat);
    w = arma::diagmat(mu_hat/(1.0 + alpha_hat[i] * mu_hat));
    z = arma::log(mu_hat / nfrow) + (yrow - mu_hat) / mu_hat;
    ridge = arma::diagmat(lambda);
    beta_hat_new = (x.t() * w * x + ridge).i() * x.t() * w * z;
    beta_hat_nv = wrap(beta_hat);
    beta_hat_new_nv = wrap(beta_hat_new);
    change = abs(beta_hat_new_nv - beta_hat_nv);
    too_large = abs(beta_hat_new_nv) > large;
    beta_hat = beta_hat_new;
    if (all(too_large).is_true()) {
      break;
    } else if (all(too_large | (change < tol)).is_true()) {
      break;
    }
  }
  last_change(i,_) = ifelse(too_large, 0.0, change);
  beta_mat.row(i) = beta_hat.t();
  beta_var_mat.row(i) = arma::diagvec((x.t() * w * x + ridge).i() * x.t() * w * x * (x.t() * w * x + ridge).i()).t();
}

return Rcpp::List::create(Rcpp::Named("beta_mat",beta_mat),
			  Rcpp::Named("beta_var_mat",beta_var_mat),
			  Rcpp::Named("iter",iter),
			  Rcpp::Named("last_change",last_change));
END_RCPP
}


