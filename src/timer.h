// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#ifndef TIMER_H_
#define TIMER_H_

#include <chrono>

/*
GAP Benchmark Suite
Class:  Timer
Authors: Scott Beamer, Michael Sutton

Simple timer that wraps std::chrono
*/

class Timer {
public:
  Timer() {}

  void Start() {
    elapsed_time_ = start_time_ = std::chrono::high_resolution_clock::now();
  }

  void Stop() { elapsed_time_ = std::chrono::high_resolution_clock::now(); }

  double Seconds() const {
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               elapsed_time_ - start_time_)
        .count();
  }

  double Millisecs() const {
    return std::chrono::duration_cast<
               std::chrono::duration<double, std::milli>>(elapsed_time_ -
                                                          start_time_)
        .count();
  }

  double Microsecs() const {
    return std::chrono::duration_cast<
               std::chrono::duration<double, std::micro>>(elapsed_time_ -
                                                          start_time_)
        .count();
  }

private:
  std::chrono::high_resolution_clock::time_point start_time_, elapsed_time_;
};

class CumulativeTimer {
public:
  CumulativeTimer() : is_running_(false) {}

  void Start() { 
    if (!is_running_) {
      start_time_ = std::chrono::high_resolution_clock::now(); 
      is_running_ = true;
    }
  }

  void Stop() {
    if (is_running_) {
      total_ += std::chrono::duration_cast<std::chrono::duration<double>>(
          std::chrono::high_resolution_clock::now() - start_time_);
      is_running_ = false;
    }
  }

  void Reset() {
    total_ = std::chrono::duration<double>{};
    is_running_ = false;
  }

  bool IsRunning() const { return is_running_; }

  double Seconds() const { return total_.count(); }

  double Millisecs() const {
    return std::chrono::duration_cast<
               std::chrono::duration<double, std::milli>>(total_)
        .count();
  }

  double Microsecs() const {
    return std::chrono::duration_cast<
               std::chrono::duration<double, std::micro>>(total_)
        .count();
  }

private:
  std::chrono::high_resolution_clock::time_point start_time_;
  std::chrono::duration<double> total_{};
  bool is_running_;
};

// Times op's execution using the timer t
#define TIME_OP(t, op)                                                         \
  {                                                                            \
    t.Start();                                                                 \
    (op);                                                                      \
    t.Stop();                                                                  \
  }

#endif // TIMER_H_
