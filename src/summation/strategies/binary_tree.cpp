#include <mpi.h>
#include <iostream>
#include <fstream>
#include <exception>
#include <vector>
#include <numeric>
#include <strings.h>
#include <cassert>
#include <cmath>
#include <unistd.h>
#include <functional>
#include <chrono>
#include <io.hpp>
#include "binary_tree.hpp"

#undef DEBUG_OUTPUT_TREE

using namespace std;
using namespace std::string_literals;

BinaryTreeSummation::BinaryTreeSummation(uint64_t rank, vector<int> &n_summands)
    : SummationStrategy(rank, n_summands),
      acquisitionDuration(std::chrono::duration<double>::zero()),
      acquisitionCount(0L),
      size(n_summands[rank]),
      begin (startIndex[rank]),
      end (begin +  size),
      rankIntersectingSummands(calculateRankIntersectingSummands()) {
#ifdef DEBUG_OUTPUT_TREE
    printf("Rank %lu has %lu summands, starting from index %lu to %lu\n", rank, size, begin, end);
#endif
}

BinaryTreeSummation::~BinaryTreeSummation() {
#ifdef ENABLE_INSTRUMENTATION
    cout << "Rank " << rank << " avg. acquisition time: "
        << acquisitionTime() / acquisitionCount << "  ns\n";
#endif
}

const uint64_t BinaryTreeSummation::parent(const uint64_t i) {
    assert(i != 0);

    // clear least significand set bit
    return i & (i - 1);
}

bool BinaryTreeSummation::isLocal(uint64_t index) const {
    return (index >= begin && index < end);
}

/** Determine which rank has the number with a given index */
uint64_t BinaryTreeSummation::rankFromIndex(uint64_t index) const {
    int64_t rankLocalIndex = index;

    for (int64_t sourceRank = 0; sourceRank < clusterSize; sourceRank++) {
        if (rankLocalIndex < n_summands[sourceRank]) {
            //cout << "Idx " << index << " is on rank " << sourceRank
            //     << " with local idx " << rankLocalIndex << endl;

            return sourceRank;
        }

        rankLocalIndex -= n_summands[sourceRank];
    }


    throw logic_error("Number cannot be found on any node");
}

double BinaryTreeSummation::acquireNumber(uint64_t index) const {
    if (isLocal(index)) {
        // We have that number locally
        return summands[index - begin];
    }

    uint64_t sourceRank = rankFromIndex(index);
    double result;
    MPI_Recv(&result, 1, MPI_DOUBLE,
            sourceRank, index, MPI_COMM_WORLD, NULL);

    return result;
}

/* Calculate all rank-intersecting summands that must be sent out because
    * their parent is non-local and located on another rank
    */
vector<uint64_t> BinaryTreeSummation::calculateRankIntersectingSummands(void) const {
    vector<uint64_t> result;

    if (rank == 0) {
        return result;
    }

    // ignore index 0
    uint64_t startIndex = (begin == 0) ? 1 : begin;
    for (uint64_t index = startIndex; index < end; index++) {
        if (!isLocal(parent(index))) {
            result.push_back(index);
        }
    }

    return result;
}

/* Sum all numbers. Will return the total sum on rank 0
    */
double BinaryTreeSummation::accumulate(void) {
    for (auto summand : rankIntersectingSummands) {
        double result = accumulate(summand);

        // TODO: figure out if this send blocks or not.
        MPI_Send(&result, 1, MPI_DOUBLE,
                rankFromIndex(parent(summand)), summand, MPI_COMM_WORLD);
    }
    double result = (rank == ROOT_RANK) ? accumulate(0) : 0.0;
    MPI_Bcast(&result, 1, MPI_DOUBLE,
              ROOT_RANK, MPI_COMM_WORLD);

    return result;
}

double BinaryTreeSummation::accumulate(uint64_t index) {
#ifdef ENABLE_INSTRUMENTATION
    auto t1 = std::chrono::high_resolution_clock::now();
#endif

    double accumulator = acquireNumber(index);

#ifdef ENABLE_INSTRUMENTATION
    auto t2 = std::chrono::high_resolution_clock::now();
    acquisitionDuration += t2 - t1;
    acquisitionCount++;
#endif

#ifdef DEBUG_OUTPUT_TREE
    if (isLocal(index))
        printf("rk%i C%i.0 = %f\n", rank, index, accumulator);
#endif

    if (!isLocal(index)) {
        return accumulator;
    }

    int lsb_index = ffs(index);
    if (lsb_index == 0) {
        // If no bits are set, iterate over all zeros
        lsb_index = 31;
    }

    // Iterate over all variants where one of the least significant zeros
    // has been replaced with a one.
    for (int j = 1; j < lsb_index; j++) {
        int zero_position = j - 1;
        uint64_t child_index = index | (1 << zero_position);

        if (child_index < globalSize) {
            double num = accumulate(child_index);
            accumulator += num;
#ifdef DEBUG_OUTPUT_TREE
            printf("rk%i C%i.%i = C%i.%i + %f\n", rank, index, j, index, j - 1, num);
#endif
        }
    }

    return accumulator;
}

const double BinaryTreeSummation::acquisitionTime(void) const {
    return std::chrono::duration_cast<std::chrono::nanoseconds> (acquisitionDuration).count();
}