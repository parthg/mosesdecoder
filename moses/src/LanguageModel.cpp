// $Id$

/***********************************************************************
Moses - statistical machine translation system
Copyright (C) 2006-2011 University of Edinburgh

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

#include <cassert>
#include <limits>
#include <iostream>
#include <memory>
#include <sstream>

#include "FFState.h"
#include "LanguageModel.h"
#include "LanguageModelImplementation.h"
#include "LanguageModelChartState.h"
#include "TypeDef.h"
#include "Util.h"
#include "Manager.h"
#include "ChartManager.h"
#include "FactorCollection.h"
#include "Phrase.h"
#include "StaticData.h"

using namespace std;

namespace Moses
{
LanguageModel::LanguageModel(ScoreIndexManager &scoreIndexManager, LanguageModelImplementation *implementation) :
  m_implementation(implementation)
{
  scoreIndexManager.AddScoreProducer(this);
#ifndef WITH_THREADS
  // ref counting handled by boost otherwise
  m_implementation->IncrementReferenceCount();
#endif
}

LanguageModel::LanguageModel(ScoreIndexManager &scoreIndexManager, LanguageModel *loadedLM) :
  m_implementation(loadedLM->m_implementation)
{
  scoreIndexManager.AddScoreProducer(this);
#ifndef WITH_THREADS
  // ref counting handled by boost otherwise
  m_implementation->IncrementReferenceCount();
#endif
}

LanguageModel::~LanguageModel()
{
#ifndef WITH_THREADS
  if(m_implementation->DecrementReferenceCount() == 0)
    delete m_implementation;
#endif
}

// don't inline virtual funcs...
size_t LanguageModel::GetNumScoreComponents() const
{
  return 1;
}

float LanguageModel::GetWeight() const
{
  size_t lmIndex = StaticData::Instance().GetScoreIndexManager().
                   GetBeginIndex(GetScoreBookkeepingID());
  return StaticData::Instance().GetAllWeights()[lmIndex];
}

void LanguageModel::CalcScore(const Phrase &phrase
                              , float &fullScore
                              , float &ngramScore) const
{

  fullScore	= 0;
  ngramScore	= 0;

  size_t phraseSize = phrase.GetSize();
  if (!phraseSize) return;

  vector<const Word*> contextFactor;
  contextFactor.reserve(GetNGramOrder());
  std::auto_ptr<FFState> state(m_implementation->NewState((phrase.GetWord(0) == m_implementation->GetSentenceStartArray()) ?
                               m_implementation->GetBeginSentenceState() : m_implementation->GetNullContextState()));
  size_t currPos = 0;
  while (currPos < phraseSize) {
    const Word &word = phrase.GetWord(currPos);

    if (word.IsNonTerminal()) {
      // do nothing. reset ngram. needed to score targbet phrases during pt loading in chart decoding
      if (!contextFactor.empty()) {
        // TODO: state operator= ?
        state.reset(m_implementation->NewState(m_implementation->GetNullContextState()));
        contextFactor.clear();
      }
    } else {
      ShiftOrPush(contextFactor, word);
      assert(contextFactor.size() <= GetNGramOrder());

      if (word == m_implementation->GetSentenceStartArray()) {
        // do nothing, don't include prob for <s> unigram
        assert(currPos == 0);
      } else {
        float partScore = m_implementation->GetValueGivenState(contextFactor, *state).score;
        fullScore += partScore;
        if (contextFactor.size() == GetNGramOrder())
          ngramScore += partScore;
      }
    }

    currPos++;
  }
}

void LanguageModel::CalcScoreChart(const Phrase &phrase
                                   , float &beginningBitsOnly
                                   , float &ngramScore) const
{
  // TODO - get rid of this function

  beginningBitsOnly	= 0;
  ngramScore	= 0;

  size_t phraseSize = phrase.GetSize();
  if (!phraseSize) return;

	// data structure for factored context phrase (history and predicted word)
  vector<const Word*> contextFactor;
  contextFactor.reserve(GetNGramOrder());

	// initialize state to BeginSentenceStat or NullContextState
  std::auto_ptr<FFState> state(m_implementation->NewState((phrase.GetWord(0) == m_implementation->GetSentenceStartArray()) ?
                               m_implementation->GetBeginSentenceState() : m_implementation->GetNullContextState()));

	// score each word
	for (size_t currPos = 0; currPos < phraseSize; currPos++) 
	{
		// add word to context
    const Word &word = phrase.GetWord(currPos);
    assert(!word.IsNonTerminal());

    ShiftOrPush(contextFactor, word);
    assert(contextFactor.size() <= GetNGramOrder());

		// score
    if (word == m_implementation->GetSentenceStartArray()) {
      // do nothing, don't include prob for <s> unigram
      assert(currPos == 0);
    } 
		else {
			// compute score for this word and update state
      float partScore = m_implementation->GetValueGivenState(contextFactor, *state).score;

			// add to nGramScore or prefixScore, depending on context size
      if (contextFactor.size() == GetNGramOrder())
        ngramScore += partScore;
      else
        beginningBitsOnly += partScore;
    }
  }
}

void LanguageModel::ShiftOrPush(vector<const Word*> &contextFactor, const Word &word) const
{
  if (contextFactor.size() < GetNGramOrder()) {
    contextFactor.push_back(&word);
  } else {
    // shift
    for (size_t currNGramOrder = 0 ; currNGramOrder < GetNGramOrder() - 1 ; currNGramOrder++) {
      contextFactor[currNGramOrder] = contextFactor[currNGramOrder + 1];
    }
    contextFactor[GetNGramOrder() - 1] = &word;
  }
}

const FFState* LanguageModel::EmptyHypothesisState(const InputType &/*input*/) const
{
  // This is actually correct.  The empty _hypothesis_ has <s> in it.  Phrases use m_emptyContextState.
  return m_implementation->NewState(m_implementation->GetBeginSentenceState());
}

FFState* LanguageModel::Evaluate(
  const Hypothesis& hypo,
  const FFState* ps,
  ScoreComponentCollection* out) const
{
  // In this function, we only compute the LM scores of n-grams that overlap a
  // phrase boundary. Phrase-internal scores are taken directly from the
  // translation option. 

	// In the case of unigram language models, there is no overlap, so we don't
  // need to do anything.
  if(GetNGramOrder() <= 1)
    return NULL;

  clock_t t=0;
  IFVERBOSE(2) {
    t  = clock();  // track time
  }

	// Empty phrase added? nothing to be done
  if (hypo.GetCurrTargetLength() == 0)
    return ps ? m_implementation->NewState(ps) : NULL;

  const size_t currEndPos = hypo.GetCurrTargetWordsRange().GetEndPos();
  const size_t startPos = hypo.GetCurrTargetWordsRange().GetStartPos();

  // 1st n-gram
  vector<const Word*> contextFactor(GetNGramOrder());
  size_t index = 0;
  for (int currPos = (int) startPos - (int) GetNGramOrder() + 1 ; currPos <= (int) startPos ; currPos++) {
    if (currPos >= 0)
      contextFactor[index++] = &hypo.GetWord(currPos);
    else {
      contextFactor[index++] = &m_implementation->GetSentenceStartArray();
    }
  }
  FFState *res = m_implementation->NewState(ps);
  float lmScore = ps ? m_implementation->GetValueGivenState(contextFactor, *res).score : m_implementation->GetValueForgotState(contextFactor, *res).score;

  // main loop
  size_t endPos = std::min(startPos + GetNGramOrder() - 2
                           , currEndPos);
  for (size_t currPos = startPos + 1 ; currPos <= endPos ; currPos++) {
    // shift all args down 1 place
    for (size_t i = 0 ; i < GetNGramOrder() - 1 ; i++)
      contextFactor[i] = contextFactor[i + 1];

    // add last factor
    contextFactor.back() = &hypo.GetWord(currPos);

    lmScore	+= m_implementation->GetValueGivenState(contextFactor, *res).score;
  }

  // end of sentence
  if (hypo.IsSourceCompleted()) {
    const size_t size = hypo.GetSize();
    contextFactor.back() = &m_implementation->GetSentenceEndArray();

    for (size_t i = 0 ; i < GetNGramOrder() - 1 ; i ++) {
      int currPos = (int)(size - GetNGramOrder() + i + 1);
      if (currPos < 0)
        contextFactor[i] = &m_implementation->GetSentenceStartArray();
      else
        contextFactor[i] = &hypo.GetWord((size_t)currPos);
    }
    lmScore	+= m_implementation->GetValueForgotState(contextFactor, *res).score;
  } 
	else 
	{
    if (endPos < currEndPos) {
      //need to get the LM state (otherwise the last LM state is fine)
      for (size_t currPos = endPos+1; currPos <= currEndPos; currPos++) {
        for (size_t i = 0 ; i < GetNGramOrder() - 1 ; i++)
          contextFactor[i] = contextFactor[i + 1];
        contextFactor.back() = &hypo.GetWord(currPos);
      }
      m_implementation->GetState(contextFactor, *res);
    }
  }
  out->PlusEquals(this, lmScore);
  IFVERBOSE(2) {
    hypo.GetManager().GetSentenceStats().AddTimeCalcLM( clock()-t );
  }
  return res;
}

FFState* LanguageModel::EvaluateChart(
  const ChartHypothesis& hypo,
  int featureID,
  ScoreComponentCollection* out) const
{
	// data structure for factored context phrase (history and predicted word)
  vector<const Word*> contextFactor;
  contextFactor.reserve(GetNGramOrder());

	// initialize language model context state
	FFState *lmState = m_implementation->NewState( m_implementation->GetNullContextState() );

	// initial language model scores
	float prefixScore = 0.0;    // not yet final for initial words (lack context)
	float finalizedScore = 0.0; // finalized, has sufficient context

  // get index map for underlying hypotheses
  const AlignmentInfo::NonTermIndexMap &nonTermIndexMap =
    hypo.GetCurrTargetPhrase().GetAlignmentInfo().GetNonTermIndexMap();

	// loop over rule
  for (size_t phrasePos = 0, wordPos = 0; 
			 phrasePos < hypo.GetCurrTargetPhrase().GetSize(); 
			 phrasePos++) 
  {
    // consult rule for either word or non-terminal
    const Word &word = hypo.GetCurrTargetPhrase().GetWord(phrasePos);

    // regular word
    if (!word.IsNonTerminal()) 
    {
			ShiftOrPush(contextFactor, word);

			// beginning of sentence symbol <s>? -> just update state
			if (word == m_implementation->GetSentenceStartArray()) 
			{
				assert(phrasePos == 0);
        delete lmState;
				lmState = m_implementation->NewState( m_implementation->GetBeginSentenceState() );
			}
			// score a regular word added by the rule
			else
			{
				updateChartScore( &prefixScore, &finalizedScore, UntransformLMScore(m_implementation->GetValueGivenState(contextFactor, *lmState).score), ++wordPos );
			}
    }

    // non-terminal, add phrase from underlying hypothesis
    else 
    {
			// look up underlying hypothesis
      size_t nonTermIndex = nonTermIndexMap[phrasePos];
      const ChartHypothesis *prevHypo = hypo.GetPrevHypo(nonTermIndex);
      size_t subPhraseLength = prevHypo->GetNumTargetTerminals();

			// special case: rule starts with non-terminal -> copy everything
			if (phrasePos == 0) {

				// get prefixScore and finalizedScore
				const LanguageModelChartState* prevState = 
					dynamic_cast<const LanguageModelChartState*>(prevHypo->GetFFState( featureID ));
				prefixScore = prevState->GetPrefixScore();
				finalizedScore = prevHypo->GetScoreBreakdown().GetScoresForProducer(this)[0] - prefixScore;

				// get language model state
        delete lmState;
				lmState = m_implementation->NewState( prevState->GetRightContext() );

				// push suffix
				int suffixPos = prevHypo->GetSuffix().GetSize() - (GetNGramOrder()-1);
				if (suffixPos < 0) suffixPos = 0; // push all words if less than order
				for(;(size_t)suffixPos < prevHypo->GetSuffix().GetSize(); suffixPos++) 
				{
					const Word &word = prevHypo->GetSuffix().GetWord(suffixPos);
					ShiftOrPush(contextFactor, word);
					wordPos++;
				}
			}

			// internal non-terminal
			else 
			{
				// score its prefix
				for(size_t prefixPos = 0; 
						prefixPos < GetNGramOrder()-1 // up to LM order window
							&& prefixPos < subPhraseLength; // up to length
						prefixPos++) 
				{
					const Word &word = prevHypo->GetPrefix().GetWord(prefixPos);
					ShiftOrPush(contextFactor, word);
					updateChartScore( &prefixScore, &finalizedScore, UntransformLMScore(m_implementation->GetValueGivenState(contextFactor, *lmState).score), ++wordPos );
				}
				
				// check if we are dealing with a large sub-phrase
				if (subPhraseLength > GetNGramOrder() - 1) 
				{
					// add its finalized language model score
					const LanguageModelChartState* prevState = 
						dynamic_cast<const LanguageModelChartState*>(prevHypo->GetFFState( featureID ));
					finalizedScore += 
						prevHypo->GetScoreBreakdown().GetScoresForProducer(this)[0] // full score
						- prevState->GetPrefixScore();                              // - prefix score

					// copy language model state
          delete lmState;
					lmState = m_implementation->NewState( prevState->GetRightContext() );

					// push its suffix
					size_t remainingWords = subPhraseLength - (GetNGramOrder()-1);						
					if (remainingWords > GetNGramOrder()-1) {
						// only what is needed for the history window
						remainingWords = GetNGramOrder()-1;
					}
					for(size_t suffixPos = prevHypo->GetSuffix().GetSize() - remainingWords;
							suffixPos < prevHypo->GetSuffix().GetSize();
							suffixPos++) {
						const Word &word = prevHypo->GetSuffix().GetWord(suffixPos);
						ShiftOrPush(contextFactor, word);						
					}
					wordPos += subPhraseLength;
				}
			}
		}			
	}
	
	// assign combined score to score breakdown
	out->Assign(this, prefixScore + finalizedScore);

	// create and return feature function state
	LanguageModelChartState *res = new LanguageModelChartState( prefixScore, lmState, hypo );
	return res;
}

void LanguageModel::updateChartScore( float *prefixScore, float *finalizedScore, float score, size_t wordPos ) const {
	if (wordPos < GetNGramOrder()) {
		*prefixScore += score;
	}
	else {
		*finalizedScore += score;
	}
}

}