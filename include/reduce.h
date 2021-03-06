#include <dll.h>
//#include <string>
#include <sharedmem.h>
#include <stdio.h>
#include <shape.h>
#include <op.h>
#include <omp.h>
#include <templatemath.h>
#include <helper_cuda.h>
#include <nd4jmalloc.h>
#include <pairwise_util.h>
#pragma once
#ifdef __CUDACC__
#include <cuda.h>
#include <cuda_runtime.h>
#endif
#ifdef __JNI__
#include <jni.h>
#endif

//an op for the kernel
namespace functions {
    namespace reduce {

/**
 * A reduce function
 * reduces a vector down to
 * a subset of itself
 * via aggregating member
 * elements.
 */
        template<typename T>
        class ReduceFunction: public functions::ops::Op<T> {
        protected:
            int extraParamsLength = 0;
            int indexBased = 1;
        public:
            virtual
#ifdef __CUDACC__
            __host__ __device__
#endif
            int getIndexBased() {
                return indexBased;
            }


            virtual
#ifdef __CUDACC__
            __host__ __device__
#endif
            ReduceFunction<T> ** extraParamsFunctions() = 0;
            virtual
#ifdef __CUDACC__
            __host__ __device__
#endif
            int getExtraParamsLength() {
                return extraParamsLength;
            }

            virtual
#ifdef __CUDACC__
            __host__ __device__
#endif
            T * createExtraParams() {
                T *ret = (T *) malloc(sizeof(T) * this->getExtraParamsLength());
                return ret;
            }


#ifdef __CUDACC__
            virtual __host__ __device__
            T * generateExtraParamsCuda(T *input,int *shapeInfo) {
                return nullptr;
            }
#endif

            /**
             * Merge the 2 inputs
             * @param old
             * @param opOutput
             * @param extraParams
             * @return
             */
            virtual
#ifdef __CUDACC__
            inline __host__  __device__

#elif defined(__GNUC__)


#endif
            T merge(T old, T opOutput, T *extraParams) = 0;

            /**
             * Op with 1 parameter
             * @param d1
             * @param extraParams
             * @return
             */
            virtual
#ifdef __CUDACC__
            inline __host__  __device__

#elif defined(__GNUC__)


#endif
            T op(T d1, T *extraParams) = 0;

            //calculate an update of the reduce operation
            /**
             * Op with 2 parameters
             * @param old
             * @param opOutput
             * @param extraParams
             * @return
             */
            virtual
#ifdef __CUDACC__
            inline __host__  __device__

#elif defined(__GNUC__)


#endif
            T update(T old, T opOutput, T *extraParams) = 0;

#ifdef __CUDACC__
            __inline__ __device__ virtual void transformCuda1D(T *dx,
                    int *xShapeInfo,
                    T *extraParams,
                    T *result,
                    int *resultShapeInfo,
                    int *dimension,
                    int dimensionLength,
                    T *reductionBuffer, UnifiedSharedMemory *manager, int *tadOnlyShapeInfo, int *tadOffsets) {

                //shared memory space for storing intermediate results
                T *sPartials = (T *) manager->getSharedReductionBuffer();

                sPartials[threadIdx.x] = this->startingValue(dx);

                __shared__ int tadLength;
                __shared__ int tadEWS;
                __shared__ int tadRank;
                __shared__ int numTads;
                if (threadIdx.x == 0) {
            	    tadLength = shape::tadLength(xShapeInfo, dimension, dimensionLength);
                    tadEWS = shape::elementWiseStride(tadOnlyShapeInfo);
                    tadRank = shape::rank(tadOnlyShapeInfo);
                    numTads = shape::length(xShapeInfo) / tadLength;
                }
                __syncthreads();

                for (int r = blockIdx.x; r < numTads; r += gridDim.x) {
                    int tadOffsetForBlock = tadOffsets[r];

                    sPartials[threadIdx.x] = this->startingValue(dx + tadOffsetForBlock);

                    for(int i = threadIdx.x; i < tadLength; i+= blockDim.x) {
                        sPartials[threadIdx.x] = this->update(sPartials[threadIdx.x],this->op(dx[tadOffsetForBlock + i * tadEWS], extraParams), extraParams);
                    }
                    __syncthreads();

                    // aggregate. do NOT reduce for elements > tadLength
                    aggregatePartials(sPartials, threadIdx.x, nd4j::math::nd4j_min<int>(blockDim.x, tadLength), extraParams);


                    __syncthreads();
                    if (threadIdx.x == 0) {
                        result[r] = this->postProcess(sPartials[threadIdx.x], tadLength, extraParams);
                    }
                }
            }


            __inline__ __device__ void execScalarCuda(
                    T *dx,
                    int *xShapeInfo,
                    T *extraParams,
                    T *result,
                    int *resultShapeInfo,
                    T *reductionBuffer,
                    UnifiedSharedMemory *manager,
                    int *tadOnlyShapeInfo) {
                int elementWiseStride = shape::elementWiseStride(xShapeInfo);

                int n = shape::length(xShapeInfo);

                int tid = blockDim.x * blockIdx.x + threadIdx.x;

                //shared memory space for storing intermediate results
                T *sPartials = (T *) manager->getSharedReductionBuffer();

                sPartials[threadIdx.x] = this->startingValue(dx);
                __syncthreads();

                if(elementWiseStride >= 1) {
                    for(int i = tid;i < n; i += (blockDim.x * gridDim.x)) {
                        //for(int i = elementWiseStride * tid;i < n; i += (blockDim.x * gridDim.x * elementWiseStride)) {
                        sPartials[threadIdx.x] = this->update(sPartials[threadIdx.x],this->op(dx[i * elementWiseStride],extraParams),extraParams); //
                    }
                }
                else {
                    int rank = shape::rank(xShapeInfo);
                    int ind2sub[MAX_RANK];

                    for(int i = tid;i < n; i += blockDim.x * gridDim.x) {
                        shape::ind2sub(rank,shape::shapeOf(xShapeInfo),i,ind2sub);
                        int offset = shape::getOffset(0,shape::shapeOf(xShapeInfo),shape::stride(xShapeInfo),ind2sub,rank);
                        sPartials[threadIdx.x] = this->update(sPartials[threadIdx.x],this->op(dx[offset],extraParams),extraParams);
                        __syncthreads();
                    }
                }

                __syncthreads();
                aggregatePartials(sPartials, threadIdx.x, nd4j::math::nd4j_min<int>(blockDim.x, n),extraParams);


                __syncthreads();

                if (gridDim.x > 1) {
                    unsigned int *tc = (unsigned int *) reductionBuffer;
                    __shared__ bool amLast;
                    int rank = shape::rank(xShapeInfo);
                    tid = threadIdx.x;
                    if (threadIdx.x == 0) {
                        reductionBuffer[blockIdx.x] = sPartials[0];//this->postProcess(sPartials[0],n,extraParams);
                    }
                    __syncthreads();
                    __threadfence();

                    if (threadIdx.x==0) {
                        unsigned int ticket = atomicInc(&tc[4096], gridDim.x);
                        amLast = (ticket == gridDim.x-1);
                    }

                    __syncthreads();

                    if (amLast) {
                        tc[4096] = 0;

                        sPartials[threadIdx.x] = this->startingValue(dx);

                        for (int i = threadIdx.x; i < gridDim.x; i += blockDim.x) {
                            sPartials[threadIdx.x] = this->update(sPartials[threadIdx.x], reductionBuffer[i] ,extraParams);
                        }
                        __syncthreads();

                        aggregatePartials(sPartials, threadIdx.x, gridDim.x, extraParams);

                        __syncthreads();
                        if (threadIdx.x == 0) {
                            result[0] = this->postProcess(sPartials[0],n,extraParams);
                        }
                    }
                } else {
                    if (threadIdx.x == 0) {
                        unsigned int *tc = (unsigned *) reductionBuffer;
                        tc[4096] = 0;
                        result[0] = this->postProcess(sPartials[0],n,extraParams);
                    }
                }
            }
            /**
             * Kernel invocation for reduce
             * @param n the length of the buffer
             * @param dx the input
             * @param xShapeInfo the shape information for the input
             * @param extraParams extra parameters (starting value,..)
             * @param result the result buffer
             * @param resultShapeInfo the shapeinformation for the result buffer
             * @param gpuInformation the gpu information (shared memory allocated,..)
             * @param dimension the dimension to do reduce along long
             * @param dimensionLength the length of the dimension buffer
             * @param postProcessOrNot whether to reduce or not
             */
             __inline__ __device__ virtual void transformCuda6D(
                    T *dx,
                    int *xShapeInfo,
                    T *extraParams,
                    T *result,
                    int *resultShapeInfo,
                    int *dimension,
                    int dimensionLength,
                    T *reductionBuffer, UnifiedSharedMemory *manager, int *tadOnlyShapeInfo, int *tadOffsets) {

                //shared memory space for storing intermediate results
                T *sPartials = (T *) manager->getSharedReductionBuffer();

                __shared__ int tadLength;
                __shared__ int tadEWS;
                __shared__ int tadRank;
                __shared__ int numTads;
                __shared__ int *tadShape;
                __shared__ int *tadStride;
                if (threadIdx.x == 0) {
            	    tadLength = shape::tadLength(xShapeInfo, dimension, dimensionLength);
                    tadEWS = shape::elementWiseStride(tadOnlyShapeInfo);
                    tadRank = shape::rank(tadOnlyShapeInfo);
                    numTads = shape::length(xShapeInfo) / tadLength;

                    tadShape = shape::shapeOf(tadOnlyShapeInfo);
                    tadStride = shape::stride(tadOnlyShapeInfo);
                }
                sPartials[threadIdx.x] = this->startingValue(dx);
                __syncthreads();

                int xCoord[3];

                for (int r = blockIdx.x; r < numTads; r += gridDim.x) {
                    int tadOffsetForBlock = tadOffsets[r];

                    sPartials[threadIdx.x] = this->startingValue(dx + tadOffsetForBlock);

                    for(int i = threadIdx.x;i < tadLength; i += blockDim.x) {
                        shape::ind2subC(tadRank, tadShape, i, xCoord);
                        int xOffset = shape::getOffset(tadOffsetForBlock, tadShape, tadStride, xCoord, tadRank);

                        sPartials[threadIdx.x] = this->update(sPartials[threadIdx.x],this->op(dx[xOffset],extraParams),extraParams);
                    }
                    __syncthreads();

                    // aggregate. do NOT reduce for elements > tadLength
                    aggregatePartials(sPartials, threadIdx.x, nd4j::math::nd4j_min<int>(blockDim.x, tadLength), extraParams);

                    __syncthreads();
                    if (threadIdx.x == 0)
                        result[r] = this->postProcess(sPartials[threadIdx.x], tadLength, extraParams);
                }
            }

            __inline__ __device__ virtual void transformCudaXD(
                    T *dx,
                    int *xShapeInfo,
                    T *extraParams,
                    T *result,
                    int *resultShapeInfo,
                    int *dimension,
                    int dimensionLength,
                    T *reductionBuffer, UnifiedSharedMemory *manager, int *tadOnlyShapeInfo, int *tadOffsets) {

                //shared memory space for storing intermediate results
                T *sPartials = (T *) manager->getSharedReductionBuffer();

//                __shared__ shape::TAD *tad;
                __shared__ int tadLength;
                __shared__ int tadEWS;
                __shared__ int tadRank;
                __shared__ int numTads;
                __shared__ int *tadShape;
                __shared__ int *tadStride;
                if (threadIdx.x == 0) {
            	    tadLength = shape::tadLength(xShapeInfo, dimension, dimensionLength);
                    tadEWS = shape::elementWiseStride(tadOnlyShapeInfo);
                    tadRank = shape::rank(tadOnlyShapeInfo);
                    numTads = shape::length(xShapeInfo) / tadLength;

                    tadShape = shape::shapeOf(tadOnlyShapeInfo);
                    tadStride = shape::stride(tadOnlyShapeInfo);
                }
                sPartials[threadIdx.x] = this->startingValue(dx);
                __syncthreads();

                int xCoord[MAX_RANK];

                for (int r = blockIdx.x; r < numTads; r += gridDim.x) {
                    int tadOffsetForBlock = tadOffsets[r];

                    sPartials[threadIdx.x] = this->startingValue(dx + tadOffsetForBlock);

                    for(int i = threadIdx.x;i < tadLength; i += blockDim.x) {
                        shape::ind2subC(tadRank, tadShape, i, xCoord);
                        int xOffset = shape::getOffset(tadOffsetForBlock, tadShape, tadStride, xCoord, tadRank);

                        sPartials[threadIdx.x] = this->update(sPartials[threadIdx.x],this->op(dx[xOffset],extraParams),extraParams);
                    }
                    __syncthreads();

                    // aggregate. do NOT reduce for elements > tadLength
                    aggregatePartials(sPartials, threadIdx.x, nd4j::math::nd4j_min<int>(blockDim.x, tadLength), extraParams);


                    __syncthreads();
                    if (threadIdx.x == 0)
                        result[r] = this->postProcess(sPartials[threadIdx.x], tadLength, extraParams);
                }
            }

            /**
             *
             * @param sPartialsRef
             * @param tid
             * @param extraParams
             */
            __device__ virtual void aggregatePartials(T *sPartials, int tid, int numItems,T *extraParams) {
                // start the shared memory loop on the next power of 2 less
                // than the block size.  If block size is not a power of 2,
                // accumulate the intermediate sums in the remainder range.
                int floorPow2 = numItems;

                if (floorPow2 & (floorPow2 - 1)) {
                    while (floorPow2 & (floorPow2 - 1)) {
                        floorPow2 &= floorPow2 - 1;
                    }
                    if (tid >= floorPow2) {
                        sPartials[tid - floorPow2] = update(sPartials[tid - floorPow2], sPartials[tid], extraParams);
                    }
                    __syncthreads();
                }
                __syncthreads();

#pragma unroll
                for (int activeThreads = floorPow2 >> 1; activeThreads; activeThreads >>= 1) {
                    if (tid < activeThreads && tid + activeThreads < numItems) {
                        sPartials[tid] = update(sPartials[tid], sPartials[tid + activeThreads], extraParams);
                    }
                    __syncthreads();
                }

            }
#endif

            virtual
#ifdef __CUDACC__
            inline __host__  __device__

#elif defined(__GNUC__)


#endif
            T postProcess(T reduction, Nd4jIndex n, T *extraParams) {
                return reduction;
            }

#ifdef __CUDACC__
            __inline__ __host__
#endif

            T aggregateBuffer(int n, T *buffer, T *extraParams) {

                T ret = buffer[0];
#pragma omp simd
                for (int i = 1; i < n; i++) {
                    ret = update(ret, buffer[i], extraParams);
                }

                return ret;
            }

            virtual
#ifdef __CUDACC__
            __host__ __device__
#endif
            ~ReduceFunction() {
            }

#ifdef __CUDACC__
            __host__ __device__
#endif

            ReduceFunction() {
            }





            /**
             * CPU implementation
             * @param x the input data
             * @param xShapeInfo the shape information for
             * the input data
             * @param extraParams the extra parameters for the problem
             * @param result the result buffer
             * @param resultShapeInfo the shape information
             */
#ifdef __CUDACC__
            __host__ __device__
#endif

            void exec(T *x,
                      int *xShapeInfo,
                      T *extraParams,
                      T *result,
                      int *resultShapeInfo) {
                T startingVal = this->execScalar(x, xShapeInfo, extraParams);
                result[0] = startingVal;

            }



            /**
             * Reduce down to 1 number
             * @param x the input
             * @param xShapeInfo the shape information
             * for the input
             * @param extraParams the extra params
             * @return
             */
#ifdef __CUDACC__
            __host__
#endif

            T execScalar(const T *x, int xElementWiseStride, Nd4jIndex length, T *extraParams) {
                T startingVal = this->startingValue(x);
                if (xElementWiseStride == 1) {
                    if (length < 8000) {
                        T local = this->startingValue(x);
#pragma omp simd
                        for (Nd4jIndex i = 0; i < length; i++) {
                            T curr = op(x[i], extraParams);
                            local = update(local, curr, extraParams);

                        }
                        local = postProcess(local, length, extraParams);

                        return local;
                    }

                    else {
                        T finalVal = startingVal;
                        BlockInformation info(length);
                        T *blocks = new T[info.chunks];
#pragma omp parallel
                        {
                            T local = this->startingValue(x);
                            for (int i = omp_get_thread_num(); i < info.chunks; i += info.threads) {
                                Nd4jIndex newOffset = (i * info.items);
                                const T *chunk = x + newOffset;
                                Nd4jIndex itemsToLoop = info.items;
                                if (newOffset >= length) {
                                    break;
                                }

                                //handle modulo case
                                if (newOffset + info.items >= length) {
                                    itemsToLoop = length - newOffset;
                                }
#pragma omp simd
                                for (Nd4jIndex j = 0; j < itemsToLoop; j++) {
                                    T curr = op(chunk[j], extraParams);
                                    local = update(local, curr, extraParams);
                                }

                            }

                            blocks[omp_get_thread_num()] = local;
                        }

#pragma omp simd
                        for(int i = 0; i < info.threads; i++) {
                            finalVal = update(finalVal,blocks[i],extraParams);
                        }


                        finalVal = postProcess(finalVal, length, extraParams);
						delete[] blocks;
                        return finalVal;

                    }

                }

                else {
                    if (length < 8000) {
                        T local = this->startingValue(x);
#pragma omp simd
                        for (Nd4jIndex i = 0; i < length; i++) {
                            T curr = op(x[i * xElementWiseStride], extraParams);
                            local = update(local, curr, extraParams);

                        }

                        local = postProcess(local, length, extraParams);

                        return local;
                    }

                    T finalVal = startingVal;
                    BlockInformation info(length);
                    T *blocks = new T[info.chunks];


#pragma omp parallel
                    {
                        T local = this->startingValue(x);
                        for (int i = omp_get_thread_num(); i < info.chunks; i += info.threads) {
                            Nd4jIndex newOffset = (i * info.items) * xElementWiseStride;
                            const T *chunk = x + newOffset;
                            Nd4jIndex itemsToLoop = info.items;
#pragma omp simd

                            for (Nd4jIndex i = 0; i < itemsToLoop; i++) {
                                T curr = op(chunk[i * xElementWiseStride], extraParams);
                                local = update(local, curr, extraParams);
                            }


                        }

                        blocks[omp_get_thread_num()] = local;


                    }

#pragma omp simd
                    for(int i = 0; i < info.threads; i++) {
                        finalVal = update(finalVal,blocks[i],extraParams);
                    }

                    finalVal = postProcess(finalVal, length, extraParams);
					delete[] blocks;
                    return finalVal;

                }

            }



            /**
             * Reduce down to 1 number
             * @param x the input
             * @param xShapeInfo the shape information
             * for the input
             * @param extraParams the extra params
             * @return
             */
#ifdef __CUDACC__
            __host__
#endif

            T execScalar(T *x, int *xShapeInfo, T *extraParams) {
                const Nd4jIndex length = shape::length(xShapeInfo);
                int xElementWiseStride = shape::elementWiseStride(xShapeInfo);
                if (xElementWiseStride >= 1) {
                    return execScalar(x, xElementWiseStride, length, extraParams);
                }
                else {
                    int shapeIter[MAX_RANK];
                    int coord[MAX_RANK];
                    int dim;
                    int xStridesIter[MAX_RANK];

                    int *xShape = shape::shapeOf(xShapeInfo);
                    int *xStride = shape::stride(xShapeInfo);
                    T start = this->startingValue(x);
                    int rank = shape::rank(xShapeInfo);

                    if (PrepareOneRawArrayIter<T>(rank,
                                                  xShape,
                                                  x,
                                                  xStride,
                                                  &rank,
                                                  shapeIter,
                                                  &x,
                                                  xStridesIter) >= 0) {

                        ND4J_RAW_ITER_START(dim, rank, coord, shapeIter); {
                                /* Process the innermost dimension */
                                const T *xIter = x;
                                start = update(start, op(xIter[0], extraParams), extraParams);
                            }
                        ND4J_RAW_ITER_ONE_NEXT(dim,
                                               rank,
                                               coord,
                                               shapeIter,
                                               x,
                                               xStridesIter);
                        start = postProcess(start, shape::length(xShapeInfo), extraParams);
                    }
                    else {
                        printf("Unable to prepare array\n");
                    }

                    return start;


                }

            }

            /**
             * Execute on the cpu
             * @param x the input data
             * @param xShapeInfo the shape information for x
             * @param extraParams the extra parameters
             * @param result the result buffer
             * @param resultShapeInfoBuffer the shape information
             * @param dimension the dimension to perform
             * the reduce along long
             * @param dimensionLength the length of the dimension buffer
             */
            virtual
#ifdef __CUDACC__
            __host__
#endif
            void exec(T *x,
                      int *xShapeInfo,
                      T *extraParams,
                      T *result,
                      int *resultShapeInfoBuffer,
                      int *dimension,
                      int dimensionLength) {
                shape::TAD tad(xShapeInfo, dimension, dimensionLength);
                tad.createTadOnlyShapeInfo();
                tad.createOffsets();
                if(tad.dimensionLength < 1)
                    return;


                int resultLength = shape::length(resultShapeInfoBuffer);

                //pre squeezed: this is for keeping the pointer to the original
                //shape information for tad offset
                //the squeezed information doesn't render the right strides for
                //tad offset
                if (resultLength == 1 || dimensionLength == shape::rank(xShapeInfo) || tad.wholeThing) {
                    result[0] = execScalar(x, xShapeInfo, extraParams);
                    return;
                }


                if(tad.wholeThing) {
                    T start = this->startingValue(x);
#pragma omp simd
                    for(int i = 0; i < shape::length(tad.tadOnlyShapeInfo); i++) {
                        start = update(start, op(x[i], extraParams), extraParams);
                    }

                    result[0] = this->postProcess(start,shape::length(tad.tadOnlyShapeInfo),extraParams);

                }
                else if(shape::elementWiseStride(tad.tadOnlyShapeInfo) > 0 && (tad.numTads == 1 || shape::isVector(tad.tadOnlyShapeInfo) ||
                                                                               shape::isScalar(tad.tadOnlyShapeInfo) || tad.wholeThing)) {

#pragma omp parallel for
                    for(int i = 0; i < resultLength; i++) {
                        T *iter = x + tad.tadOffsets[i];
                        T start = this->startingValue(iter);
                        int eleStride = shape::elementWiseStride(tad.tadOnlyShapeInfo);
                        if(eleStride == 1) {
#pragma omp simd
                            for(int j = 0; j < shape::length(tad.tadOnlyShapeInfo); j++) {
                                start = update(start, op(iter[j], extraParams), extraParams);

                            }
                        }
                        else {
#pragma omp simd
                            for(int j = 0; j < shape::length(tad.tadOnlyShapeInfo); j++) {
                                start = update(start, op(iter[j * eleStride], extraParams), extraParams);
                            }
                        }

                        result[i] = this->postProcess(start,shape::length(tad.tadOnlyShapeInfo),extraParams);

                    }
                }
                else {
#pragma omp  parallel  for
                    for (int i = 0; i <  resultLength; i++) {
                        int offset = tad.tadOffsets[i];
                        int shapeIter[MAX_RANK];
                        int coord[MAX_RANK];
                        int dim;
                        int rankIter = shape::rank(tad.tadOnlyShapeInfo);
                        int xStridesIter[MAX_RANK];
                        T *xPointer = x + offset;
                        T start = this->startingValue(xPointer);
                        if (PrepareOneRawArrayIter<T>(rankIter,
                                                      shape::shapeOf(tad.tadOnlyShapeInfo),
                                                      xPointer,
                                                      shape::stride(tad.tadOnlyShapeInfo),
                                                      &rankIter,
                                                      shapeIter,
                                                      &xPointer,
                                                      xStridesIter) >= 0) {
                            ND4J_RAW_ITER_START(dim, shape::rank(tad.tadOnlyShapeInfo), coord, shapeIter); {
                                    /* Process the innermost dimension */
                                    if(xPointer > 0)
                                        start = update(start, op(xPointer[0], extraParams), extraParams);
                                    else printf("Xpointer is < 0\n");
                                }
                            ND4J_RAW_ITER_ONE_NEXT(dim,
                                                   rankIter,
                                                   coord,
                                                   shapeIter,
                                                   xPointer,
                                                   xStridesIter);
                            start = postProcess(start, shape::length(tad.tadOnlyShapeInfo), extraParams);
                        }
                        else {
                            printf("Unable to prepare array\n");
                        }

                        result[i] = start;
                    }
                }



            }

            virtual inline
#ifdef __CUDACC__
            __host__ __device__
#endif
            void aggregateExtraParams(T **extraParamsTotal,T **extraParamsLocal) {
                // no extra params aggregation needs to happen
            }

            virtual
#ifdef __CUDACC__
            __host__ __device__
#endif
            T startingValue(const T *input) = 0;




        };

#ifdef __CUDACC__
        /**
    *
    * @param extraParams
    * @param sPartials
    * @param sMemSize
    */
        template<typename T>
        __device__ void initializeShared(T *extraParams, T **sPartials, int sMemSize) {
            int sPartialsLength = sMemSize / sizeof(T);
            T *sPartialsDeref = (T *) *sPartials;
            for (int i = 0; i < sPartialsLength; i++) {
                sPartialsDeref[i] = extraParams[0];
            }
        }

#endif

        namespace ops {
/**
 * Summation operation
 */
            template<typename T>
            class Sum: public virtual functions::reduce::ReduceFunction<T> {
            public:
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                T startingValue(const T *input) override {
                    (void)input;
                    return (T) 0.0;
                }
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ReduceFunction<T> ** extraParamsFunctions() {
                    return nullptr;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T merge(T old, T opOutput, T *extraParams) override {
                    return opOutput + old;
                }
                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T update(T old, T opOutput, T *extraParams) override {
                    return opOutput + old;
                }
                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T op(T d1, T *extraParams) override {
                    return d1;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    return reduction;
                }

                virtual
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                ~Sum() {
                }
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                Sum() {
                }
            };

/**
 * The product operation
 */
            template<typename T>
            class Prod: public virtual functions::reduce::ReduceFunction<T> {
            public:

                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ReduceFunction<T> ** extraParamsFunctions() {
                    return nullptr;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T merge(T old, T opOutput, T *extraParams) override {
                    return opOutput * old;
                }
                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T update(T old, T opOutput, T *extraParams) override {
                    return opOutput * old;
                }
                virtual
#ifdef __CUDACC__
                __host__  __device__

#elif defined(__GNUC__)


#endif
                T op(T d1, T *extraParams) override {
                    return d1;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    return reduction;
                }

                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                T startingValue(const T *input) override {
                    (void)input;
                    return 1.0;
                }


                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ~Prod() {
                }
#ifdef __CUDACC__
                __host__ __device__
#endif
                Prod() {
                }
            };

/**
 * Mean operation
 */
            template<typename T>
            class Mean: public virtual functions::reduce::ReduceFunction<T> {
            public:
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ReduceFunction<T> ** extraParamsFunctions() {
                    return nullptr;
                }
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                T startingValue(const T *input) override {
                    (void)input;
                    return 0.0;
                }


                virtual
#ifdef __CUDACC__
                __host__  __device__

#elif defined(__GNUC__)


#endif
                T merge(T old, T opOutput, T *extraParams) override {
                    return opOutput + old;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T update(T old, T opOutput, T *extraParams) override {
                    return opOutput + old;
                }
                virtual
#ifdef __CUDACC__
                __host__  __device__

#elif defined(__GNUC__)


#endif
                T op(T d1, T *extraParams) override {
                    return d1;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    return reduction / (T) n;
                }

                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ~Mean() {
                }
#ifdef __CUDACC__
                __host__ __device__
#endif
                Mean() {
                }
            };


/**
 * Max reduction
 */
            template<typename T>
            class Max: public virtual functions::reduce::ReduceFunction<T> {
            public:

                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ReduceFunction<T> ** extraParamsFunctions() {
                    return nullptr;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T merge(T old, T opOutput, T *extraParams) override {
                    return nd4j::math::nd4j_max<T>(old, opOutput);
                }
                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T update(T old, T opOutput, T *extraParams) override {
                    return nd4j::math::nd4j_max<T>(opOutput, old);
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T op(T d1, T *extraParams) override {
                    return d1;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    return reduction;
                }


                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                T startingValue(const T *input) override {
                    return input[0];
                }

                virtual
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                ~Max() {
                }
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                Max() {
                    this->indexBased = 1;
                }
            };

/**
 * Min operation
 */
            template<typename T>
            class Min: public virtual functions::reduce::ReduceFunction<T> {
            public:

                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ReduceFunction<T> ** extraParamsFunctions() {
                    return nullptr;
                }


                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T merge(T old, T opOutput, T *extraParams) override {
                    return nd4j::math::nd4j_min<T>(old, opOutput);
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T update(T old, T opOutput, T *extraParams) override {
                    return nd4j::math::nd4j_min<T>(opOutput, old);
                }
                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T op(T d1, T *extraParams) override {
                    return d1;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    return reduction;
                }

                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                T startingValue(const T *input) override {
                    return input[0];
                }


                virtual
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                ~Min() {
                }
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                Min() {
                    this->indexBased = 1;
                }
            };

/**
 * Norm1 of a buffer
 */
            template<typename T>
            class Norm1: public virtual functions::reduce::ReduceFunction<T> {
            public:
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ReduceFunction<T> ** extraParamsFunctions() {
                    return nullptr;
                }
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                T startingValue(const T *input) override {
                    (void)input;
                    return 0.0;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T merge(T old, T opOutput, T *extraParams) override {
                    return opOutput + old;

                }
                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T update(T old, T opOutput, T *extraParams) override {
                    return opOutput + old;

                }
                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T op(T d1, T *extraParams) override {
                    return nd4j::math::nd4j_abs<T>(d1);
                }

                virtual
#ifdef __CUDACC__
                __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    return reduction;
                }

                virtual
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                ~Norm1() {}
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                Norm1() {}
            };

/**
 * Norm2 of an array
 */
            template<typename T>
            class Norm2: public virtual functions::reduce::ReduceFunction<T> {
            public:
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                T startingValue(const T *input) override {
                    (void)input;
                    return 0.0;
                }
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ReduceFunction<T> ** extraParamsFunctions() {
                    return nullptr;
                }


                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T merge(T old, T opOutput, T *extraParams) override {
                    return opOutput + old;

                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T update(T old, T opOutput, T *extraParams) override {
                    return opOutput + old;

                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T op(T d1, T *extraParams) override {
                    return d1 * d1;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    return nd4j::math::nd4j_sqrt<T>(reduction);
                }

                virtual
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                ~Norm2() {
                }
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                Norm2() {
                }
            };

/**
 * Norm max of an array
 */
            template<typename T>
            class NormMax: public virtual functions::reduce::ReduceFunction<T> {
            public:
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                T startingValue(const T *input) {
                    (void)input;
                    return 0.0;
                }
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ReduceFunction<T> ** extraParamsFunctions() {
                    return nullptr;
                }


                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T merge(T old, T opOutput, T *extraParams) override {
                    return opOutput + old;

                }

                virtual
#ifdef __CUDACC__
                __host__  __device__

#elif defined(__GNUC__)


#endif
                T update(T old, T opOutput, T *extraParams) override {
                    return nd4j::math::nd4j_max<T>(nd4j::math::nd4j_abs<T>(old),
                                                   nd4j::math::nd4j_abs<T>(opOutput));

                }
                virtual
#ifdef __CUDACC__
                __host__  __device__

#elif defined(__GNUC__)


#endif
                T op(T d1, T *extraParams) override {
                    return d1;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    return nd4j::math::nd4j_max<T>(nd4j::math::nd4j_abs<T>(reduction),
                                                   nd4j::math::nd4j_abs<T>(reduction));
                }

                virtual
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                ~NormMax() {
                }

#ifdef __CUDACC__
                inline __host__ __device__
#endif
                NormMax() {
                }
            };

            template<typename T>
            class Variance: public  functions::reduce::ReduceFunction<T> {
            public:
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                T startingValue(const T *input) override {
                    (void)input;
                    return 0.0;
                }
                virtual
#ifdef __CUDACC__
                __host__ __device__
#endif
                ReduceFunction<T> ** extraParamsFunctions() {
                    return nullptr;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T merge(T old, T opOutput, T *extraParams) override {
                    return old + opOutput;

                }
                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T update(T old, T opOutput, T *extraParams) override {
                    return old + opOutput;

                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__


#elif defined(__GNUC__)


#endif
                T op(T d1, T *extraParams) override {
                    T mean = extraParams[0];
                    T ret = d1 - mean;
                    return ret * ret;
                }

                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    T bias = extraParams[1];
                    return (reduction - (nd4j::math::nd4j_pow<T>(bias, 2.0) / (T) n))
                           / (T) (n - 1.0);
                }

                virtual
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                ~Variance() {
                }
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                Variance() {
                    this->extraParamsLength = 2;
                }
            };

/**
 * Standard deviation of a buffer
 */
            template<typename T>
            class StandardDeviation: public virtual Variance<T> {
            public:


                virtual
#ifdef __CUDACC__
                inline __host__  __device__

#elif defined(__GNUC__)


#endif
                T postProcess(T reduction, Nd4jIndex n,T *extraParams) override {
                    T ret = Variance<T>::postProcess(reduction,n,extraParams);
                    T sqrtRet = nd4j::math::nd4j_sqrt<T>(ret);
                    return sqrtRet;
                }

                virtual
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                ~StandardDeviation() {
                }
#ifdef __CUDACC__
                inline __host__ __device__
#endif
                StandardDeviation() : Variance<T>() {
                }
            };



        }

        template<typename T>
        class ReduceOpFactory: public virtual functions::ops::OpFactory<T> {

        public:
#ifdef __CUDACC__
            __device__ __host__
#endif
            ReduceOpFactory() {
            }

            /**
             * Create an operation given an op number
             * @param op the operation number
             * 0: mean
             * 1: sum
             * 2: bias
             * 3: max
             * 4: min
             * 5: norm1
             * 6: norm2
             * 7: normmaxc
             * 8: prod
             * 9: std
             * 10: variance
             * @return
             */
#ifdef __CUDACC__
            __inline__ __device__
            virtual functions::reduce::ReduceFunction<T> * create(int op, unsigned char *buffer) {

#else
                virtual functions::reduce::ReduceFunction<T> * create(int op) {
#endif
                if (op == 0)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::Mean<T>();
#else
                    return new functions::reduce::ops::Mean<T>();
#endif
                else if (op == 1)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::Sum<T>();
#else
                    return new functions::reduce::ops::Sum<T>();
#endif
                else if (op == 3)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::Max<T>();
#else
                    return new functions::reduce::ops::Max<T>();
#endif
                else if (op == 4)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::Min<T>();
#else
                    return new functions::reduce::ops::Min<T>();
#endif
                else if (op == 5)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::Norm1<T>();
#else
                    return new functions::reduce::ops::Norm1<T>();
#endif
                else if (op == 6)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::Norm2<T>();
#else
                    return new functions::reduce::ops::Norm2<T>();
#endif
                else if (op == 7)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::NormMax<T>();
#else
                    return new functions::reduce::ops::NormMax<T>();
#endif
                else if (op == 8)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::Prod<T>();
#else
                    return new functions::reduce::ops::Prod<T>();
#endif
                else if (op == 9)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::StandardDeviation<T>();
#else
                    return new functions::reduce::ops::StandardDeviation<T>();
#endif
                else if (op == 10)
#ifdef __CUDACC__
                    return new(buffer) functions::reduce::ops::Variance<T>();
#else
                return new functions::reduce::ops::Variance<T>();
#endif
                return nullptr;
            }


#ifdef __CUDACC__
            __device__ __host__
#endif

            virtual ~ReduceOpFactory() {
            }
        };

    }

}


#ifdef __CUDACC__

/**
 * Interface for the c and driver api
 * @param op the operation number
 * @param n the length of the problem
 * @param dx  the input information
 * @param xShapeInfo the shape information
 * @param extraParams the extra parameters
 * @param result the result data
 * @param resultShapeInfo the result shape information
 * @param gpuInformation the gpu information
 * @param dimension the dimension to do reduce along long
 * @param dimensionLength the length of the dimension buffer
 * @param postProcessOrNot whether to pre process or not
 */
template <typename T>
__device__ void reduceGeneric(
        int op,
        T *dx,
        int *xShapeInfo,
        T *extraParams,
        T *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        T *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {

    __shared__ functions::reduce::ReduceFunction<T> *reduceFunctionToInvoke;

    __shared__ UnifiedSharedMemory *manager;

    if (threadIdx.x == 0) {
        extern __shared__ unsigned char shmem[];
        manager = new(shmem) UnifiedSharedMemory((int *) shmem);
        manager->init(sizeof(UnifiedSharedMemory), sizeof(functions::reduce::ReduceOpFactory<T>), sizeof(functions::reduce::ops::Max<T>), sizeof(shape::TAD), shape::rank(xShapeInfo));
    }
    __syncthreads();

    if(threadIdx.x == 0) {
        functions::reduce::ReduceOpFactory<T> *newOpFactory =  new(manager->getFactorySpace()) functions::reduce::ReduceOpFactory<T>();
        reduceFunctionToInvoke = newOpFactory->create(op, manager->getFunctionSpace());
    }
    __syncthreads();
    reduceFunctionToInvoke->transformCudaXD(
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, manager, tadOnlyShapeInfo, tadOffsets);
}

template <typename T>
__device__ void reduceGeneric1D(
        int op,
        T *dx,
        int *xShapeInfo,
        T *extraParams,
        T *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        T *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {

    __shared__ functions::reduce::ReduceFunction<T> *reduceFunctionToInvoke;

    __shared__ UnifiedSharedMemory *manager;

    if (threadIdx.x == 0) {
        extern __shared__ unsigned char shmem[];
        manager = new(shmem) UnifiedSharedMemory((int *) shmem);
        manager->init(sizeof(UnifiedSharedMemory), sizeof(functions::reduce::ReduceOpFactory<T>), sizeof(functions::reduce::ops::Max<T>), sizeof(shape::TAD), shape::rank(xShapeInfo));

        functions::reduce::ReduceOpFactory<T> *newOpFactory =  new(manager->getFactorySpace()) functions::reduce::ReduceOpFactory<T>();
        reduceFunctionToInvoke = newOpFactory->create(op, manager->getFunctionSpace());
    }
    __syncthreads();
    reduceFunctionToInvoke->transformCuda1D(
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, manager, tadOnlyShapeInfo, tadOffsets);
}

template <typename T>
__device__ void reduceGeneric6D(
        int op,
        T *dx,
        int *xShapeInfo,
        T *extraParams,
        T *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        T *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {

    __shared__ functions::reduce::ReduceFunction<T> *reduceFunctionToInvoke;


    extern __shared__ unsigned char shmem[];
    __shared__ UnifiedSharedMemory *manager;

    if (threadIdx.x == 0) {
        manager = new(shmem) UnifiedSharedMemory((int *) shmem);
        manager->init(sizeof(UnifiedSharedMemory), sizeof(functions::reduce::ReduceOpFactory<T>), sizeof(functions::reduce::ops::Max<T>), 16, shape::rank(xShapeInfo));

        //
        functions::reduce::ReduceOpFactory<T> *newOpFactory =  new(manager->getFactorySpace()) functions::reduce::ReduceOpFactory<T>();
        reduceFunctionToInvoke = newOpFactory->create(op, manager->getFunctionSpace());
    }
    __syncthreads();


    reduceFunctionToInvoke->transformCuda6D(
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, manager, tadOnlyShapeInfo, tadOffsets);

}

/**
 * Interface for the c and driver api
 * @param op the operation number
 * @param n the length of the problem
 * @param dx  the input information
 * @param xShapeInfo the shape information
 * @param extraParams the extra parameters
 * @param result the result data
 * @param resultShapeInfo the result shape information
 * @param gpuInformation the gpu information
 * @param dimension the dimension to do reduce along long
 * @param dimensionLength the length of the dimension buffer
 * @param postProcessOrNot whether to pre process or not
 */
extern "C" __global__ void reduceDouble(
        int op,
        double *dx,
        int *xShapeInfo,
        double *extraParams,
        double *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        double *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric<double>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo, tadOffsets);

}

extern "C" __global__ void reduceDouble1D(
        int op,
        double *dx,
        int *xShapeInfo,
        double *extraParams,
        double *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        double *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric1D<double>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo, tadOffsets);

}

extern "C" __global__ void reduceDouble6D(
        int op,
        double *dx,
        int *xShapeInfo,
        double *extraParams,
        double *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        double *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric6D<double>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo, tadOffsets);

}

/**
 * Interface for the c and driver api
 * @param op the operation number
 * @param n the length of the problem
 * @param dx  the input information
 * @param xShapeInfo the shape information
 * @param extraParams the extra parameters
 * @param result the result data
 * @param resultShapeInfo the result shape information
 * @param gpuInformation the gpu information
 * @param dimension the dimension to do reduce along long
 * @param dimensionLength the length of the dimension buffer
 * @param postProcessOrNot whether to pre process or not
 */
extern "C" __global__ void reduceFloat(
        int op,
        float *dx,
        int *xShapeInfo,
        float *extraParams,
        float *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric<float>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo, tadOffsets);
}

extern "C" __global__ void reduceFloat1D(
        int op,
        float *dx,
        int *xShapeInfo,
        float *extraParams,
        float *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric1D<float>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo, tadOffsets);
}


extern "C" __global__ void reduceFloat6D(
        int op,
        float *dx,
        int *xShapeInfo,
        float *extraParams,
        float *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float *reductionBuffer, int *tadOnlyShapeInfo, int *tadOffsets) {
    reduceGeneric6D<float>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo, tadOffsets);
}

template <typename T>
__device__ void reduceScalarGeneric(
        int op,
        T *dx,
        int *xShapeInfo,
        T *extraParams,
        T *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        T *reductionBuffer, int *tadOnlyShapeInfo) {
    __shared__ functions::reduce::ReduceFunction<T> *reduceFunctionToInvoke;
    __shared__ UnifiedSharedMemory *manager;

    if (threadIdx.x == 0) {
        extern __shared__ unsigned char shmem[];
        manager = new(shmem) UnifiedSharedMemory((int *) shmem);
        manager->init(sizeof(UnifiedSharedMemory), sizeof(functions::reduce::ReduceOpFactory<T>), sizeof(functions::reduce::ops::Max<T>), sizeof(shape::TAD), 0);

        functions::reduce::ReduceOpFactory<T> *newOpFactory =  new(manager->getFactorySpace()) functions::reduce::ReduceOpFactory<T>();
        reduceFunctionToInvoke = newOpFactory->create(op, manager->getFunctionSpace());
    }
    __syncthreads();
    reduceFunctionToInvoke->execScalarCuda(
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            reductionBuffer, manager, tadOnlyShapeInfo);
};

extern "C" __global__ void reduceScalarFloat(
        int op,
        float *dx,
        int *xShapeInfo,
        float *extraParams,
        float *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        float *reductionBuffer, int *tadOnlyShapeInfo) {
    reduceScalarGeneric<float>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo);
};

extern "C" __global__ void reduceScalarDouble(
        int op,
        double *dx,
        int *xShapeInfo,
        double *extraParams,
        double *result,
        int *resultShapeInfo,
        int *dimension,
        int dimensionLength,
        double *reductionBuffer, int *tadOnlyShapeInfo) {
    reduceScalarGeneric<double>(
            op,
            dx,
            xShapeInfo,
            extraParams,
            result,
            resultShapeInfo,
            dimension,
            dimensionLength,
            reductionBuffer, tadOnlyShapeInfo);
};
#endif

