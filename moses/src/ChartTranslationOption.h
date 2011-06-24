// $Id$
/***********************************************************************
 Moses - factored phrase-based language decoder
 Copyright (C) 2010 Hieu Hoang

 This library is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This library is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this library; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 ***********************************************************************/

#pragma once

#include <cassert>
#include <vector>
#include "Word.h"
#include "WordsRange.h"
#include "TargetPhrase.h"

namespace Moses
{
class CoveredChartSpan;
class ChartCellCollection;

// basically a phrase translation and the vector of words consumed to map each word
class ChartTranslationOption
{
  friend std::ostream& operator<<(std::ostream&, const ChartTranslationOption&);

protected:
  const TargetPhrase &m_targetPhrase;
  const CoveredChartSpan &m_lastCoveredChartSpan;
  /* map each source word in the phrase table to:
  		1. a word in the input sentence, if the pt word is a terminal
  		2. a 1+ phrase in the input sentence, if the pt word is a non-terminal
  */
  const WordsRange	&m_wordsRange;

    float m_estimateOfBestScore;

  ChartTranslationOption &operator=(const ChartTranslationOption &);  // not implemented

  void CalcEstimateOfBestScore(const CoveredChartSpan *, const ChartCellCollection &);

public:
  ChartTranslationOption(const TargetPhrase &targetPhrase, const CoveredChartSpan &lastCoveredChartSpan, const WordsRange	&wordsRange, const ChartCellCollection &allChartCells)
    :m_targetPhrase(targetPhrase)
    ,m_lastCoveredChartSpan(lastCoveredChartSpan)
    ,m_wordsRange(wordsRange)
    ,m_estimateOfBestScore(m_targetPhrase.GetFutureScore())
  {
    CalcEstimateOfBestScore(&m_lastCoveredChartSpan, allChartCells);
  }

  ~ChartTranslationOption()
  {}

  const TargetPhrase &GetTargetPhrase() const {
    return m_targetPhrase;
  }

  const CoveredChartSpan &GetLastCoveredChartSpan() const {
    return m_lastCoveredChartSpan;
  }

  const WordsRange &GetSourceWordsRange() const {
    return m_wordsRange;
  }

  // return an estimate of the best score possible with this translation option.
  // the estimate is the sum of the target phrase's estimated score plus the
  // scores of the best child hypotheses.  (the same as the ordering criterion
  // currently used in RuleCubeQueue.)
  inline float GetEstimateOfBestScore() const {
    return m_estimateOfBestScore;
  }

};

}