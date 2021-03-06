/*!
 *  Copyright (c) 2015 by Contributors
 * \file wlibsvm_parser.h
 * \brief iterator parser to parse libsvm format
 * \author Tianqi Chen
 */
#ifndef XGBOOST_IO_WLIBSVM_PARSER_H_
#define XGBOOST_IO_WLIBSVM_PARSER_H_
#define NOMINMAX
#include <vector>
#include <cstring>
#include <cctype>
#include <algorithm>
#include "../utils/omp.h"
#include "../utils/utils.h"
#include "../sync/sync.h"
#include "../utils/thread_buffer.h"
#include "./sparse_batch_page.h"
#include "../utils/fmap.h"

namespace xgboost {
namespace io {
/*! \brief page returned by wlibsvm parser */
struct WLibSVMPage : public SparsePage {
  std::vector<float> label;
  std::vector<float> weight;
  // overload clear
  inline void Clear() {
    SparsePage::Clear();
    label.clear();
    weight.clear();
  }
};

/*!
 * \brief libsvm parser that parses the input lines
 * and returns rows in input data
 * factry that was used by threadbuffer template
 */
class WLibSVMPageFactory  {
 public:
  explicit WLibSVMPageFactory()
      : bytes_read_(0), at_head_(true) {
  }

  inline bool Init() {
    return true;
  }
  inline void Setup(dmlc::InputSplit *source,
                    int nthread, utils::FeatMap *fmap = NULL) {
    source_ = source;
    int maxthread;
    #pragma omp parallel
    {
      maxthread = omp_get_num_procs();
    }
    maxthread = std::max(maxthread / 2, 1);
    nthread_ = std::min(maxthread, nthread);
    fmap_ = fmap;

  }
  inline void SetParam(const char *name, const char *val) {}
  inline bool LoadNext(std::vector<WLibSVMPage> *data) {
    return FillData(data);
  }
  inline void FreeSpace(std::vector<WLibSVMPage> *a) {
    delete a;
  }
  inline std::vector<WLibSVMPage> *Create(void) {
    return new std::vector<WLibSVMPage>();
  }
  inline void BeforeFirst(void) {
    utils::Assert(at_head_, "cannot call beforefirst");
  }
  inline void Destroy(void) {
    delete source_;
  }
  inline size_t bytes_read(void) const {
    return bytes_read_;
  }

 protected:
  inline bool FillData(std::vector<WLibSVMPage> *data) {
    dmlc::InputSplit::Blob chunk;
    if (!source_->NextChunk(&chunk)) return false;
    int nthread;
    #pragma omp parallel num_threads(nthread_)
    {
      nthread = omp_get_num_threads();
    }
    // reserve space for data
    data->resize(nthread);
    bytes_read_ += chunk.size;
    utils::Assert(chunk.size != 0, "WLibSVMParser.FileData");
    char *head = reinterpret_cast<char*>(chunk.dptr);
    #pragma omp parallel num_threads(nthread_)
    {
      // threadid
      int tid = omp_get_thread_num();
      size_t nstep = (chunk.size + nthread - 1) / nthread;
      size_t sbegin = std::min(tid * nstep, chunk.size);
      size_t send = std::min((tid + 1) * nstep, chunk.size);
      char *pbegin = BackFindEndLine(head + sbegin, head);
      char *pend;
      if (tid + 1 == nthread) {
        pend = head + send;
      } else {
        pend = BackFindEndLine(head + send, head);
      }
      ParseBlock(pbegin, pend, &(*data)[tid]);
    }
    return true;
  }
  /*!
   * \brief parse data into out
   * \param begin beginning of buffer
   * \param end end of buffer
   */
  inline void ParseBlock(char *begin,
                         char *end,
                         WLibSVMPage *out) {
    using namespace std;
    out->Clear();
    char *p = begin;

    while (p != end) {
      while (isspace(*p) && p != end) ++p;
      if (p == end) break;
      p = ParseOneRecord(p,out);
      if (out->label.size() != 0) {
          out->offset.push_back(out->data.size());
      }
      while(isspace(*p) && p != end) ++p;
    }

    if (out->label.size() != 0) {
      out->offset.push_back(out->data.size());
    }
    utils::Check(out->label.size() + 1 == out->offset.size(),
                 "WLibSVMParser inconsistent");
  }

  inline char* ParseOneRecord(char *begin, WLibSVMPage *out){
      // get weight
      char *p = begin;
      out->weight.push_back(static_cast<float>(atof(p)));

      // get label
      while(!isspace(*p)) ++p;
      out->label.push_back(static_cast<float>(atof(p)));
      while(isspace(*p)) ++p;
      while(!isspace(*p)) ++p;
      if (*p == '\r' || *p == '\n') return p;
      //get features
      while(*p != '\r' && *p != '\n') {
          while(isspace(*p) && *p != '\r' && *p != '\n') ++p;
          if (*p == '\r' || *p == '\n') return p;
          int index = atol(p);
          while (isdigit(*p)) ++p;
          if (*p == ':'){
              if(NULL == fmap_ || !fmap_->contain_exclude_feature(index)){
              out->data.push_back(SparseBatch::Entry(index, static_cast<bst_float>(atof(p + 1))));
          }
          }
          while(!isspace(*p)) ++p;
      }
      return p;
  }

  /*!
   * \brief start from bptr, go backward and find first endof line
   * \param bptr end position to go backward
   * \param begin the beginning position of buffer
   * \return position of first endof line going backward
   */
  inline char* BackFindEndLine(char *bptr,
                               char *begin) {
    for (; bptr != begin; --bptr) {
      if (*bptr == '\n' || *bptr == '\r') return bptr;
    }
    return begin;
  }

 private:
  // nthread
  int nthread_;
  // number of bytes readed
  size_t bytes_read_;
  // at beginning, at end of stream
  bool at_head_;
  // source split that provides the data
  dmlc::InputSplit *source_;

  //fmap
  utils::FeatMap *fmap_;

};

class WLibSVMParser : public utils::IIterator<WLibSVMPage> {
 public:
  explicit WLibSVMParser(dmlc::InputSplit *source,
                        int nthread, utils::FeatMap *fmap = NULL)
      : at_end_(false), data_ptr_(0), data_(NULL) {
    itr.SetParam("buffer_size", "2");
    itr.get_factory().Setup(source, nthread, fmap);
    itr.Init();
  }
  virtual void BeforeFirst(void) {
    itr.BeforeFirst();
  }
  virtual bool Next(void) {
    if (at_end_) return false;
    while (true) {
      if (data_ == NULL || data_ptr_ >= data_->size()) {
        if (!itr.Next(data_)) {
          at_end_ = true; return false;
        } else {
          data_ptr_ = 0;
        }
      }
      while (data_ptr_ < data_->size()) {
        data_ptr_ += 1;
        if ((*data_)[data_ptr_ - 1].Size() != 0) {
          return true;
        }
      }
    }
    return true;
  }
  virtual const WLibSVMPage &Value(void) const {
    return (*data_)[data_ptr_ - 1];
  }
  inline size_t bytes_read(void) const {
    return itr.get_factory().bytes_read();
  }
 private:
  bool at_end_;
  size_t data_ptr_;
  std::vector<WLibSVMPage> *data_;
  utils::ThreadBuffer<std::vector<WLibSVMPage>*, WLibSVMPageFactory> itr;
};

}  // namespace io
}  // namespace xgboost
#endif  // XGBOOST_IO_LIBSVM_PARSER_H_
