#include <string>

#include "model_manager.h"
#include "state_space_gaussian_model_manager.h"
#include "state_space_regression_model_manager.h"
#include "state_space_logit_model_manager.h"
#include "state_space_poisson_model_manager.h"
#include "state_space_student_model_manager.h"
#include "utils.h"

#include "r_interface/boom_r_tools.hpp"
#include "r_interface/list_io.hpp"
#include "r_interface/create_state_model.hpp"

#include "Models/StateSpace/StateModels/DynamicRegressionStateModel.hpp"
#include "Models/StateSpace/Filters/KalmanTools.hpp"

#include "cpputil/report_error.hpp"
#include "distributions.hpp"

namespace BOOM {
namespace bsts {

ModelManager * ModelManager::Create(SEXP r_bsts_object) {
  std::string family = ToString(getListElement(
      r_bsts_object, "family"));
  bool regression = !Rf_isNull(getListElement(
      r_bsts_object, "predictors"));
  int xdim = 0;
  if (regression) {
    xdim = Rf_ncols(getListElement(r_bsts_object, "predictors"));
  }
  return ModelManager::Create(family, xdim);
}

ModelManager * ModelManager::Create(const std::string &family_name,
                                    int xdim) {
  ModelManager *ans = nullptr;
  if (family_name == "gaussian") {
    if (xdim > 0) {
      StateSpaceRegressionModelManager *manager =
          new StateSpaceRegressionModelManager;
      manager->SetPredictorDimension(xdim);
      ans = manager;
    } else {
      ans = new StateSpaceModelManager;
    }
  } else if (family_name == "logit") {
    StateSpaceLogitModelManager *manager =
        new StateSpaceLogitModelManager;
    manager->SetPredictorDimension(xdim);
    ans = manager;
  } else if (family_name == "poisson") {
    StateSpacePoissonModelManager *manager =
        new StateSpacePoissonModelManager;
    manager->SetPredictorDimension(xdim);
    ans = manager;
  } else if (family_name == "student") {
    StateSpaceStudentModelManager *manager
        = new StateSpaceStudentModelManager;
    manager->SetPredictorDimension(xdim);
    ans = manager;
  } else {
    std::ostringstream err;
    err << "Unrecognized family name: " << family_name
        << " in ModelManager::Create.";
    report_error(err.str());
    return nullptr;
  }
  return ans;
}

StateSpaceModelBase * ModelManager::CreateModel(
    SEXP r_data_list,
    SEXP r_state_specification,
    SEXP r_prior,
    SEXP r_options,
    Vector *final_state,
    bool save_state_contribution,
    bool save_prediction_errors,
    RListIoManager *io_manager) {
  StateSpaceModelBase *model = CreateObservationModel(
      r_data_list,
      r_prior,
      r_options,
      io_manager);

  RInterface::StateModelFactory state_model_factory(io_manager, model);
  state_model_factory.AddState(r_state_specification);
  state_model_factory.SaveFinalState(final_state);

  if (save_state_contribution) {
    io_manager->add_list_element(
        new NativeMatrixListElement(
            new GeneralStateContributionCallback(model),
            "state.contributions",
            NULL));
  }

  if (save_prediction_errors) {
    // The final NULL argument is because we will not be streaming
    // prediction errors in future calculations.  They are for
    // reporting only.  As usual, the rows of the matrix correspond to
    // MCMC iterations, so the columns represent time.
    io_manager->add_list_element(
        new BOOM::NativeVectorListElement(
            new PredictionErrorCallback(model),
            "one.step.prediction.errors",
            NULL));
  }
  return model;
}

Matrix ModelManager::Forecast(SEXP r_bsts_object,
                              SEXP r_prediction_data,
                              SEXP r_burn,
                              SEXP r_observed_data) {
  RListIoManager io_manager;
  Vector final_state;
  SEXP r_state_specfication = getListElement(
      r_bsts_object, "state.specification");
  StateSpaceModelBase *model = CreateModel(
      R_NilValue,
      r_state_specfication,
      R_NilValue,
      R_NilValue,
      &final_state,
      false,
      false,
      &io_manager);
  bool refilter;
  if (Rf_isNull(r_observed_data)) {
    AddDataFromBstsObject(r_bsts_object);
    refilter = false;
  } else {
    AddDataFromList(r_observed_data);
    refilter = true;
  }
  int niter = Rf_asInteger(getListElement(r_bsts_object, "niter"));
  int burn = std::max<int>(0, Rf_asInteger(r_burn));
  io_manager.prepare_to_stream(r_bsts_object);
  io_manager.advance(burn);
  int iterations_after_burnin = niter - burn;

  if (Rf_isNull(r_prediction_data)) {
    report_error("Forecast called with NULL prediction data.");
  }
  int forecast_horizon = UnpackForecastData(r_prediction_data);
  UnpackDynamicRegressionForecastData(
      model, r_state_specfication, r_prediction_data);

  Matrix ans(iterations_after_burnin, forecast_horizon);
  for (int i = 0; i < iterations_after_burnin; ++i) {
    io_manager.stream();
    if (refilter) {
      const ScalarKalmanStorage &storage(model->filter());
      Vector state_mean = storage.a;
      SpdMatrix state_variance = storage.P;
      make_contemporaneous(
          state_mean,
          state_variance,
          storage.F,
          storage.v,
          model->observation_matrix(model->time_dimension()).dense());
      final_state = rmvn(state_mean, state_variance);
    }
    ans.row(i) = SimulateForecast(final_state);
  }
  return ans;
}

Matrix ModelManager::OneStepPredictionErrors(
    SEXP r_bsts_object,
    SEXP r_holdout_data,
    SEXP r_burn) {
  RListIoManager io_manager;
  Vector final_state;
  StateSpaceModelBase *model = CreateModel(R_NilValue,
              getListElement(r_bsts_object, "state.specification"),
              R_NilValue,
              R_NilValue,
              &final_state,
              false,
              false,
              &io_manager);
  int niter = Rf_asInteger(getListElement(r_bsts_object, "niter"));
  int burn = std::max<int>(0, Rf_asInteger(r_burn));
  io_manager.prepare_to_stream(r_bsts_object);
  io_manager.advance(burn);
  int iterations_after_burnin = niter - burn;

  bool holdout = Rf_isNull(r_holdout_data);
  int time_horizon = -1;
  if (holdout) {
    time_horizon = UnpackHoldoutData(r_holdout_data);
  } else {
    time_horizon = ToBoomVector(getListElement(
        r_bsts_object, "original.series")).size();
  }
  Matrix ans(iterations_after_burnin, time_horizon);
  for (int i = 0; i < iterations_after_burnin; ++i) {
    io_manager.stream();
    if (holdout) {
      ans.row(i) = HoldoutDataOneStepHoldoutPredictionErrors(final_state);
    } else {
      ans.row(i) = model->one_step_prediction_errors();
    }
  }
  return ans;
}

void ModelManager::UnpackDynamicRegressionForecastData(
    StateSpaceModelBase *model,
    SEXP r_state_specification,
    SEXP r_prediction_data) {
  if (Rf_length(r_state_specification) < model->nstate()) {
    std::ostringstream err;
    err << "The number of state components in the model: ("
        << model->nstate() << ") does not match the size of "
        << "the state specification: ("
        << Rf_length(r_state_specification)
        << ") in UnpackDynamicRegressionForecastData.";
    report_error(err.str());
  }
  for (int i = 0; i < model->nstate(); ++i) {
    SEXP spec = VECTOR_ELT(r_state_specification, i);
    if (Rf_inherits(spec, "DynamicRegression")) {
      Matrix predictors = ToBoomMatrix(getListElement(
          r_prediction_data, "dynamic.regression.predictors"));
      Ptr<StateModel> state_model = model->state_model(i);
      state_model.dcast<DynamicRegressionStateModel>()->add_forecast_data(
          predictors);
    }
  }
}

}  // namespace bsts
}  // namespace BOOM
