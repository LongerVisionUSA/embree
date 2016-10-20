// ======================================================================== //
// Copyright 2009-2016 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "../common/default.h"

namespace embree
{
  /* serial partitioning */
  template<typename T, typename V, typename Compare, typename Reduction_T>
    __forceinline size_t serial_partitioning(T* array, 
                                             const size_t begin,
                                             const size_t end, 
                                             V& leftReduction,
                                             V& rightReduction,
                                             const Compare& cmp, 
                                             const Reduction_T& reduction_t)
  {
    T* l = array + begin;
    T* r = array + end - 1;
    
    while(1)
    {
      /* *l < pivot */
      while (likely(l <= r && cmp(*l) )) 
      {
#if defined(__AVX512F__)
        prefetch<PFHINT_L1EX>(l+4);	  
#endif
        reduction_t(leftReduction,*l);
        ++l;
      }
      /* *r >= pivot) */
      while (likely(l <= r && !cmp(*r)))
      {
#if defined(__AVX512F__)
        prefetch<PFHINT_L1EX>(r-4);	  
#endif
        reduction_t(rightReduction,*r);
        --r;
      }
      if (r<l) break;
      
      reduction_t(leftReduction ,*r);
      reduction_t(rightReduction,*l);
      xchg(*l,*r);
      l++; r--;
    }
    
    return l - array;        
  }
  
  template<size_t BLOCK_SIZE, typename T, typename V, typename Compare, typename Reduction_T, typename Reduction_V>
    class __aligned(64) parallel_partition_static_task
  {
    ALIGNED_CLASS;
  private:

    struct Range 
    {
      ssize_t start;
      ssize_t end;

      __forceinline Range() {}

      __forceinline Range (ssize_t start, ssize_t end) 
      : start(start), end(end) {}

      __forceinline void reset() { 
        start = 0; end = -1; 
      } 
	
      __forceinline Range intersect(const Range& r) const {
        return Range (max(start,r.start),min(end,r.end)); // carefull with ssize_t here
      }

      __forceinline bool empty() const { 
        return end < start; 
      } 
	
      __forceinline size_t size() const { 
        assert(!empty());
        return end-start+1; 
      }
    };

  private:

    static const size_t MAX_TASKS = 512;

    T* array;
    size_t N;
    size_t tasks; 
    const Compare& cmp;
    const Reduction_T& reduction_t;
    const Reduction_V& reduction_v;
    const V &init;

    size_t numMisplacedRangesLeft;
    size_t numMisplacedRangesRight;
    size_t numMisplacedItems;

    __aligned(64) size_t counter_start[MAX_TASKS]; 
    __aligned(64) size_t counter_left[MAX_TASKS];  
    __aligned(64) Range leftMisplacedRanges[MAX_TASKS];  
    __aligned(64) Range rightMisplacedRanges[MAX_TASKS]; 
    __aligned(64) V leftReductions[MAX_TASKS];           
    __aligned(64) V rightReductions[MAX_TASKS];    

  public:
     
    __forceinline parallel_partition_static_task(T *array, 
                                                 const size_t N, 
                                                 const size_t maxNumThreads,
                                                 const V& init, 
                                                 const Compare& cmp, 
                                                 const Reduction_T& reduction_t, 
                                                 const Reduction_V& reduction_v) 
      : array(array), N(N), cmp(cmp), reduction_t(reduction_t), reduction_v(reduction_v), init(init)
    {
      numMisplacedRangesLeft  = 0;
      numMisplacedRangesRight = 0;
      numMisplacedItems  = 0;
      tasks = (N+maxNumThreads-1)/maxNumThreads >= BLOCK_SIZE ? maxNumThreads : (N+BLOCK_SIZE-1)/BLOCK_SIZE;
      tasks = min(tasks,MAX_TASKS);
    }

    __forceinline const Range *findStartRange(size_t &index,const Range *const r,const size_t numRanges)
    {
      size_t i = 0;
      while(index >= r[i].size())
      {
        assert(i < numRanges);
        index -= r[i].size();
        i++;
      }	    
      return &r[i];
    }

    __forceinline void swapItemsInMisplacedRanges(const Range * const leftMisplacedRanges,
                                                  const size_t numLeftMisplacedRanges,
                                                  const Range * const rightMisplacedRanges,
                                                  const size_t numRightMisplacedRanges,
                                                  const size_t startID,
                                                  const size_t endID)
    {

      size_t leftLocalIndex  = startID;
      size_t rightLocalIndex = startID;

      const Range* l_range = findStartRange(leftLocalIndex,leftMisplacedRanges,numLeftMisplacedRanges);
      const Range* r_range = findStartRange(rightLocalIndex,rightMisplacedRanges,numRightMisplacedRanges);

      size_t l_left = l_range->size() - leftLocalIndex;
      size_t r_left = r_range->size() - rightLocalIndex;

      size_t size = endID - startID;

      T *__restrict__ l = &array[l_range->start + leftLocalIndex];
      T *__restrict__ r = &array[r_range->start + rightLocalIndex];

      size_t items = min(size,min(l_left,r_left)); 

      while(size)
      {
        if (unlikely(l_left == 0))
        {
          l_range++;
          l_left = l_range->size();
          l = &array[l_range->start];
          items = min(size,min(l_left,r_left));

        }

        if (unlikely(r_left == 0))
        {		
          r_range++;
          r_left = r_range->size();
          r = &array[r_range->start];          
          items = min(size,min(l_left,r_left));
        }

        size   -= items;
        l_left -= items;
        r_left -= items;

        while(items) {
          items--;
          xchg(*l++,*r++);
        }
      }
    }


    __forceinline size_t partition(V &leftReduction,
                                   V &rightReduction)
    {
      if (unlikely(N < BLOCK_SIZE))
      {
        leftReduction = empty;
        rightReduction = empty;
        return serial_partitioning(array,0,N,leftReduction,rightReduction,cmp,reduction_t);
      }

      parallel_for(tasks,[&] (const size_t taskID) {
          const size_t startID = (taskID+0)*N/tasks;
          const size_t endID   = (taskID+1)*N/tasks;
          V local_left(empty);
          V local_right(empty);
          const size_t mid = serial_partitioning(array,startID,endID,local_left,local_right,cmp,reduction_t);
          counter_start[taskID] = startID;
          counter_left [taskID] = mid-startID;
          leftReductions[taskID]  = local_left;
          rightReductions[taskID] = local_right;
        });
      
      leftReduction = empty;
      rightReduction = empty;

      for (size_t i=0;i<tasks;i++)
      {
        reduction_v(leftReduction,leftReductions[i]);
        reduction_v(rightReduction,rightReductions[i]);
      }

      numMisplacedRangesLeft  = 0;
      numMisplacedRangesRight = 0;
      size_t numMisplacedItemsLeft   = 0;
      size_t numMisplacedItemsRight  = 0;
	
      counter_start[tasks] = N;
      counter_left[tasks]  = 0;

      size_t mid = counter_left[0];
      for (size_t i=1;i<tasks;i++)
        mid += counter_left[i];

      const Range globalLeft (0,mid-1);
      const Range globalRight(mid,N-1);

      for (size_t i=0;i<tasks;i++)
      {	    
        const size_t left_start  = counter_start[i];
        const size_t left_end    = counter_start[i] + counter_left[i]-1;
        const size_t right_start = counter_start[i] + counter_left[i];
        const size_t right_end   = counter_start[i+1]-1;

        Range left_range (left_start,left_end);
        Range right_range(right_start,right_end);

        Range left_misplaced = globalLeft.intersect(right_range);
        Range right_misplaced = globalRight.intersect(left_range);

        if (!left_misplaced.empty())  
        {
          numMisplacedItemsLeft  += left_misplaced.size();
          leftMisplacedRanges[numMisplacedRangesLeft++] = left_misplaced;
        }

        if (!right_misplaced.empty()) 
        {
          numMisplacedItemsRight += right_misplaced.size();
          rightMisplacedRanges[numMisplacedRangesRight++] = right_misplaced;
        }
      }

      assert( numMisplacedItemsLeft == numMisplacedItemsRight );
	
      numMisplacedItems = numMisplacedItemsLeft;

      const size_t global_mid = mid;


      ////////////////////////////////////////////////////////////////////////////
      ////////////////////////////////////////////////////////////////////////////
      ////////////////////////////////////////////////////////////////////////////

      if (numMisplacedItems)
      {

        parallel_for(tasks,[&] (const size_t taskID) {
            const size_t startID = (taskID+0)*numMisplacedItems/tasks;
            const size_t endID   = (taskID+1)*numMisplacedItems/tasks;
            swapItemsInMisplacedRanges(leftMisplacedRanges,
                                       numMisplacedRangesLeft,
                                       rightMisplacedRanges,
                                       numMisplacedRangesRight,
                                       startID,
                                       endID);	                             
          });
      }

      return global_mid;
    }

  };


  template<size_t BLOCK_SIZE, typename T, typename V, typename Compare, typename Reduction_T, typename Reduction_V>
    __forceinline size_t parallel_in_place_partitioning_static(T *array, 
                                                               const size_t N, 
                                                               const V &init,
                                                               V &leftReduction,
                                                               V &rightReduction,
                                                               const Compare& cmp, 
                                                               const Reduction_T& reduction_t,
                                                               const Reduction_V& reduction_v,
                                                               const size_t numThreads = TaskScheduler::threadCount())
  {
#if defined(__X86_64__) 
    typedef parallel_partition_static_task<BLOCK_SIZE, T,V,Compare,Reduction_T,Reduction_V> partition_task;
    std::unique_ptr<partition_task> p(new partition_task(array,N,numThreads,init,cmp,reduction_t,reduction_v));
    return p->partition(leftReduction,rightReduction);    
#else
    return serial_partitioning(array,size_t(0),N,leftReduction,rightReduction,cmp,reduction_t);
#endif
  }

}
