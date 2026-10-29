/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_struct.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else {
    ObDatum token;
    int64_t token_freq = 0;
    bool found_alnum = false;
    uint32_t begin = 0;
    uint32_t end = 0;
    // Keep the basic English normalizer's byte-based edge trimming. In
    // particular, non-ASCII bytes must not be reclassified as Unicode letters.
    while (OB_SUCC(ret) && !found_alnum) {
      token.reset();
      if (OB_FAIL(tokenizer_.get_next(token, token_freq))) {
      } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 == token.len_ || token_freq <= 0)) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        begin = 0;
        end = token.len_;
        while (begin < end && !ob_isalnum(cs_, token.ptr_[begin])) {
          ++begin;
        }
        found_alnum = begin < end;
        while (end > begin && !ob_isalnum(cs_, token.ptr_[end - 1])) {
          --end;
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObString normalized;
      // Allocate the normalized token directly in the caller's owning arena.
      // The word map retains these bytes after the next token is requested.
      const char *source = token.ptr_ + begin;
      const uint32_t length = end - begin;
      bool ascii = cs_->number == CS_TYPE_UTF8MB4_GENERAL_CI || cs_->number == CS_TYPE_UTF8MB4_BIN;
      for (uint32_t i = 0; ascii && i < length; ++i) {
        ascii = static_cast<unsigned char>(source[i]) < 0x80;
      }
      if (ascii) {
        char *buffer = static_cast<char *>(allocator_.alloc(length));
        if (OB_ISNULL(buffer)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          for (uint32_t i = 0; i < length; ++i) {
            const char c = source[i];
            buffer[i] = c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
          }
          normalized.assign_ptr(buffer, length);
        }
      } else if (OB_FAIL(ObCharset::tolower(cs_, ObString(length, source), normalized, allocator_))) {
      }
      if (OB_SUCC(ret)) {
        word = normalized.ptr();
        word_len = normalized.length();
        char_len = word_len;
        word_freq = token_freq;
      }
    }
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    doc_.set_string(param->fulltext_, param->ft_length_);
    cs_ = param->cs_;
    if (OB_FAIL(tokenizer_.open(doc_, cs_))) {
    } else {
      is_inited_ = true;
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  tokenizer_.reset();
  cs_ = nullptr;
  doc_.reset();
  is_inited_ = false;
}

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObBEngFTParser *parser = nullptr;
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, param->allocator_, *(param->allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser->init(param))) {
  } else {
    iter = parser;
  }

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObBEngFTParser, param->allocator_, parser);
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter)) {
    abort_unless(nullptr != param);
    abort_unless(nullptr != param->allocator_);
    iter->~ObITokenIterator();
    param->allocator_->free(iter);
  }
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
