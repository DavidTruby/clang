// RUN: %clang_cc1 -verify -fopenmp -fopenmp-version=45 -x c++ -std=c++11 -triple powerpc64le-ibm-linux-gnu -fopenmp-targets=powerpc64le-ibm-linux-gnu -emit-llvm %s -o - | FileCheck %s
// expected-no-diagnostics

template <typename T1, typename T2>
struct pair {
  T1 t1;
  T2 t2;
  pair(T1 t1, T2 t2) : t1(t1), t2(t2) {}
};

template <typename T1, typename T2>
pair<T1, T2> make_pair(T1 &&t1, T2 &&t2) {
  return {t1, t2};
}

// CHECK-LABEL: @main
int main(int argc, char **argv) {
// CHECK: call i32 @__tgt_target(i64 -1, i8* [[OFFLOAD:@[^.]+]].region_id, i32 0, i8** null, i8** null, i64* null, i64* null)
#pragma omp target
 {
    for (int i = 0; i < 64; ++i) {
      for (int j = 0; j < 64; ++j) {
        auto foo = make_pair(i * i, j * j);
      }
    }
  }
  return 0;
}

// CHECK: define internal void [[OFFLOAD]](
// CHECK: call {{.+}} @{{.*}}make_pair