/*
 * FakeMutexGuard.h
 *
 *  Created on: Jul 21, 2022
 *      Author: jim
 */

#ifndef _FAKEMUTEXGUARD_H_
#define _FAKEMUTEXGUARD_H_

#include <atomic>

namespace pilecv4j
{
namespace ffmpeg
{

/**
 * This uses an atomic<bool> as a NON-recursive mutex. When the bool
 * has the value of 'false', it's considered "unlocked." Locking it
 * by instantiating a FakeMutexGuard will atomically transform it to
 * false.
 */
class FakeMutextGuard {
  // false means it's free. true means I need to wait.
  std::atomic<bool>& fmut;

public:
  inline FakeMutextGuard(std::atomic<bool>& pfmut) : fmut(pfmut) {

    bool mfalse = false;
    while(!fmut.compare_exchange_weak(mfalse,true)) {
      mfalse = false;
    }
  }

  inline ~FakeMutextGuard() {
    bool mtrue = true;
    while(!fmut.compare_exchange_weak(mtrue, false)) {
      mtrue = true;
    }
  }
};



}
} /* namespace pilecv4j */

#endif /* SRC_MAIN_CPP_UTILS_FAKEMUTEXGUARD_H_ */
