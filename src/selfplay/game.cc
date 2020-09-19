/*
  This file is part of Leela Chess Zero.
  Copyright (C) 2018 The LCZero Authors

  Leela Chess is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Leela Chess is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with Leela Chess.  If not, see <http://www.gnu.org/licenses/>.

  Additional permission under GNU GPL version 3 section 7

  If you modify this Program, or any covered work, by linking or
  combining it with NVIDIA Corporation's libraries from the NVIDIA CUDA
  Toolkit and the NVIDIA CUDA Deep Neural Network library (or a
  modified version of those libraries), containing parts covered by the
  terms of the respective license agreement, the licensors of this
  Program grant you additional permission to convey the resulting work.
*/

#include "selfplay/game.h"

#include <algorithm>

#include "mcts/stoppers/factory.h"
#include "mcts/stoppers/stoppers.h"
#include "neural/writer.h"

namespace lczero {

lczero::Move ply_to_lc0_move(pgn::Ply& ply, const lczero::ChessBoard& board,
                             bool mirror) {
  if (ply.isShortCastle() || ply.isLongCastle()) {
    lczero::Move m;
    int file_to = ply.isShortCastle() ? 7 : 0;
    unsigned int rowIndex = mirror ? 7 : 0;
    m.SetFrom(lczero::BoardSquare(rowIndex, 4));
    m.SetTo(lczero::BoardSquare(rowIndex, file_to));
    return m;
  } else {
    for (auto legal_move : board.GenerateLegalMoves()) {
      const bool knight_move = board.our_knights().get(legal_move.from());
      const bool bishop_move = board.bishops().get(legal_move.from());
      const bool rook_move = board.rooks().get(legal_move.from());
      const bool queen_move = board.queens().get(legal_move.from());
      const bool king_move = board.our_king().get(legal_move.from());
      const bool pawn_move = board.pawns().get(legal_move.from());

      if (mirror) {
        legal_move.Mirror();
      }

      if (legal_move.to().row() == ply.toSquare().rowIndex() &&
          legal_move.to().col() == ply.toSquare().colIndex()) {
        auto piece = ply.piece();
        if (piece == pgn::Piece::Knight() && !knight_move ||
            piece == pgn::Piece::Bishop() && !bishop_move ||
            piece == pgn::Piece::Rook() && !rook_move ||
            piece == pgn::Piece::Queen() && !queen_move ||
            piece == pgn::Piece::King() && !king_move ||
            piece == pgn::Piece::Pawn() && !pawn_move) {
          continue;
        }

        int colIndex = ply.fromSquare().colIndex();
        if (colIndex >= 0 && legal_move.from().col() != colIndex) {
          continue;
        }

        int rowIndex = ply.fromSquare().rowIndex();
        if (rowIndex >= 0 && legal_move.from().row() != rowIndex) {
          continue;
        }

        if (ply.promotion()) {
          switch (ply.promoted().letter()) {
            case 'Q':
              legal_move.SetPromotion(lczero::Move::Promotion::Queen);
              break;
            case 'R':
              legal_move.SetPromotion(lczero::Move::Promotion::Rook);
              break;
            case 'B':
              legal_move.SetPromotion(lczero::Move::Promotion::Bishop);
              break;
            case 'N':
              legal_move.SetPromotion(lczero::Move::Promotion::Knight);
            default:
              assert(false);
          }
        }

        // Had to comment this to get working for this use case. Not sure why it
        // has to be commented though...
        /*
        if (mirror) {
          legal_move.Mirror();
        }
        */
        return legal_move;
      }
    }
  }
  throw Exception("Didn't understood move: " + ply.str());
  return {};
}

namespace {
const OptionId kReuseTreeId{"reuse-tree", "ReuseTree",
                            "Reuse the search tree between moves."};
const OptionId kResignPercentageId{
    "resign-percentage", "ResignPercentage",
    "Resign when win percentage drops below specified value."};
const OptionId kResignWDLStyleId{
    "resign-wdlstyle", "ResignWDLStyle",
    "If set, resign percentage applies to any output state being above "
    "100% minus the percentage instead of winrate being below."};
const OptionId kResignEarliestMoveId{"resign-earliest-move",
                                     "ResignEarliestMove",
                                     "Earliest move that resign is allowed."};
const OptionId kMinimumAllowedVistsId{
    "minimum-allowed-visits", "MinimumAllowedVisits",
    "Unless the selected move is the best move, temperature based selection "
    "will be retried until visits of selected move is greater than or equal to "
    "this threshold."};
const OptionId kUciChess960{
    "chess960", "UCI_Chess960",
    "Castling moves are encoded as \"king takes rook\"."};
}  // namespace

void SelfPlayGame::PopulateUciParams(OptionsParser* options) {
  options->Add<BoolOption>(kReuseTreeId) = false;
  options->Add<BoolOption>(kResignWDLStyleId) = false;
  options->Add<FloatOption>(kResignPercentageId, 0.0f, 100.0f) = 0.0f;
  options->Add<IntOption>(kResignEarliestMoveId, 0, 1000) = 0;
  options->Add<IntOption>(kMinimumAllowedVistsId, 0, 1000000) = 0;
  options->Add<BoolOption>(kUciChess960) = false;
  PopulateTimeManagementOptions(RunType::kSelfplay, options);
}

SelfPlayGame::SelfPlayGame(PlayerOptions player1, PlayerOptions player2,
                           bool shared_tree, const MoveList& opening)
    : options_{player1, player2},
      chess960_{player1.uci_options->Get<bool>(kUciChess960.GetId()) ||
                player2.uci_options->Get<bool>(kUciChess960.GetId())} {
  
  std::string whiteNonPawns = "rnbqkbnr";
  std::string blackNonPawns = "RNBQKBNR";
  random_shuffle(whiteNonPawns.begin(), whiteNonPawns.end());
  random_shuffle(blackNonPawns.begin(), blackNonPawns.end());
  std::string startPosFen = whiteNonPawns + "/pppppppp/8/8/8/8/PPPPPPPP/" + blackNonPawns + " w - - 0 1";

  tree_[0] = std::make_shared<NodeTree>();
  tree_[0]->ResetToPosition(startPosFen, {});

  if (shared_tree) {
    tree_[1] = tree_[0];
  } else {
    tree_[1] = std::make_shared<NodeTree>();
    tree_[1]->ResetToPosition(startPosFen, {});
  }
  for (Move m : opening) {
    tree_[0]->MakeMove(m);
    if (tree_[0] != tree_[1]) tree_[1]->MakeMove(m);
  }
}

void SelfPlayGame::Play(int white_threads, int black_threads, bool training,
                        bool enable_resign, SyzygyTablebase* syzygy_tb,
                        pgn::Game* opening) {
  bool blacks_move = (tree_[0]->GetPlyCount() % 2) == 1;

  pgn::MoveList openingMovelist = opening ? opening->moves() : pgn::MoveList();
  auto openingMove = openingMovelist.begin();

  // Do moves while not end of the game. (And while not abort_)
  while (!abort_) {
    game_result_ = tree_[0]->GetPositionHistory().ComputeGameResult();

    // If endgame, stop.
    if (game_result_ != GameResult::UNDECIDED) break;

    // Initialize search.
    const int idx = blacks_move ? 1 : 0;
    const bool inBook = {
        openingMove != openingMovelist.end() &&
        (blacks_move ? openingMove->black() : openingMove->white()).valid()
    };
    if (!options_[idx].uci_options->Get<bool>(kReuseTreeId.GetId())) {
      tree_[idx]->TrimTreeAtHead();
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (abort_) break;
      auto stoppers = options_[idx].search_limits.MakeSearchStopper();
      PopulateIntrinsicStoppers(stoppers.get(), options_[idx].uci_options);

      std::unique_ptr<UciResponder> responder =
          std::make_unique<CallbackUciResponder>(
              options_[idx].best_move_callback, options_[idx].info_callback);

      if (!chess960_) {
        // Remap FRC castling to legacy castling.
        responder = std::make_unique<Chess960Transformer>(
            std::move(responder), tree_[idx]->HeadPosition().GetBoard());
      }

      search_ = std::make_unique<Search>(
          *tree_[idx], options_[idx].network, std::move(responder),
          /* searchmoves */ MoveList(), std::chrono::steady_clock::now(),
          std::move(stoppers),
          /* infinite */ false, *options_[idx].uci_options, options_[idx].cache,
          syzygy_tb);
    }

    // Do search.
    search_->RunBlocking(blacks_move ? black_threads : white_threads);
    move_count_++;
    nodes_total_ += search_->GetTotalPlayouts();
    if (abort_) break;

    auto best_eval = search_->GetBestEval();
    if (training) {
      // Append training data. The GameResult is later overwritten.
      auto best_q = best_eval.first;
      auto best_d = best_eval.second;
      training_data_.push_back(tree_[idx]->GetCurrentHead()->GetV4TrainingData(
          GameResult::UNDECIDED, tree_[idx]->GetPositionHistory(),
          search_->GetParams().GetHistoryFill(), best_q, best_d));
    }

    float eval = best_eval.first;
    eval = (eval + 1) / 2;
    if (eval < min_eval_[idx]) min_eval_[idx] = eval;
    const int move_number = tree_[0]->GetPositionHistory().GetLength() / 2 + 1;
    auto best_w = (best_eval.first + 1.0f - best_eval.second) / 2.0f;
    auto best_d = best_eval.second;
    auto best_l = best_w - best_eval.first;
    max_eval_[0] = std::max(max_eval_[0], blacks_move ? best_l : best_w);
    max_eval_[1] = std::max(max_eval_[1], best_d);
    max_eval_[2] = std::max(max_eval_[2], blacks_move ? best_w : best_l);
    if (enable_resign && move_number >= options_[idx].uci_options->Get<int>(
                                            kResignEarliestMoveId.GetId())) {
      const float resignpct =
          options_[idx].uci_options->Get<float>(kResignPercentageId.GetId()) /
          100;
      if (options_[idx].uci_options->Get<bool>(kResignWDLStyleId.GetId())) {
        auto threshold = 1.0f - resignpct;
        if (best_w > threshold) {
          game_result_ =
              blacks_move ? GameResult::BLACK_WON : GameResult::WHITE_WON;
          break;
        }
        if (best_l > threshold) {
          game_result_ =
              blacks_move ? GameResult::WHITE_WON : GameResult::BLACK_WON;
          break;
        }
        if (best_d > threshold) {
          game_result_ = GameResult::DRAW;
          break;
        }
      } else {
        if (eval < resignpct) {  // always false when resignpct == 0
          game_result_ =
              blacks_move ? GameResult::WHITE_WON : GameResult::BLACK_WON;
          break;
        }
      }
    }

    Move move;
    while (true) {
      move = search_->GetBestMove().first;
      uint32_t max_n = 0;
      uint32_t cur_n = 0;
      for (auto edge : tree_[idx]->GetCurrentHead()->Edges()) {
        if (edge.GetN() > max_n) {
          max_n = edge.GetN();
        }
        if (edge.GetMove(tree_[idx]->IsBlackToMove()) == move) {
          cur_n = edge.GetN();
        }
      }
      // If 'best move' is less than allowed visits and not max visits,
      // discard it and try again.
      if (cur_n == max_n ||
          static_cast<int>(cur_n) >= options_[idx].uci_options->Get<int>(
                                         kMinimumAllowedVistsId.GetId())) {
        break;
      }
      PositionHistory history_copy = tree_[idx]->GetPositionHistory();
      Move move_for_history = move;
      if (tree_[idx]->IsBlackToMove()) {
        move_for_history.Mirror();
      }
      history_copy.Append(move_for_history);
      // Ensure not to discard games that are already decided.
      if (history_copy.ComputeGameResult() == GameResult::UNDECIDED) {
        auto move_list_to_discard = GetMoves();
        move_list_to_discard.push_back(move);
        options_[idx].discarded_callback(move_list_to_discard);
      }
      search_->ResetBestMove();
    }

    // Add best (or book) move to the tree.
    lczero::Move move_;
    if (inBook) {
      auto pgn_move = blacks_move ? openingMove->black() : openingMove->white();
      move_ = ply_to_lc0_move(pgn_move,
                             tree_[idx]->GetPositionHistory().Last().GetBoard(),
                             blacks_move);
    } else {
      move_ = search_->GetBestMove().first;
    }

    if (openingMove != openingMovelist.end() && blacks_move) {
      openingMove++;
    }

    tree_[0]->MakeMove(move_);
    if (tree_[0] != tree_[1]) tree_[1]->MakeMove(move_);
    blacks_move = !blacks_move;
  }
}

std::vector<Move> SelfPlayGame::GetMoves() const {
  std::vector<Move> moves;
  for (Node* node = tree_[0]->GetCurrentHead();
       node != tree_[0]->GetGameBeginNode(); node = node->GetParent()) {
    moves.push_back(node->GetParent()->GetEdgeToNode(node)->GetMove());
  }
  std::vector<Move> result;
  Position pos = tree_[0]->GetPositionHistory().Starting();
  while (!moves.empty()) {
    Move move = moves.back();
    moves.pop_back();
    if (!chess960_) move = pos.GetBoard().GetLegacyMove(move);
    pos = Position(pos, move);
    // Position already flipped, therefore flip the move if white to move.
    if (!pos.IsBlackToMove()) move.Mirror();
    result.push_back(move);
  }
  return result;
}

float SelfPlayGame::GetWorstEvalForWinnerOrDraw() const {
  // TODO: This assumes both players have the same resign style.
  // Supporting otherwise involves mixing the meaning of worst.
  if (options_[0].uci_options->Get<bool>(kResignWDLStyleId.GetId())) {
    if (game_result_ == GameResult::WHITE_WON) {
      return std::max(max_eval_[1], max_eval_[2]);
    } else if (game_result_ == GameResult::BLACK_WON) {
      return std::max(max_eval_[1], max_eval_[0]);
    } else {
      return std::max(max_eval_[2], max_eval_[0]);
    }
  }
  if (game_result_ == GameResult::WHITE_WON) return min_eval_[0];
  if (game_result_ == GameResult::BLACK_WON) return min_eval_[1];
  return std::min(min_eval_[0], min_eval_[1]);
}

void SelfPlayGame::Abort() {
  std::lock_guard<std::mutex> lock(mutex_);
  abort_ = true;
  if (search_) search_->Abort();
}

void SelfPlayGame::WriteTrainingData(TrainingDataWriter* writer) const {
  for (auto chunk : training_data_) {
    const bool black_to_move = chunk.side_to_move;
    if (game_result_ == GameResult::WHITE_WON) {
      chunk.result = black_to_move ? -1 : 1;
    } else if (game_result_ == GameResult::BLACK_WON) {
      chunk.result = black_to_move ? 1 : -1;
    } else {
      chunk.result = 0;
    }
    writer->WriteChunk(chunk);
  }
}

std::unique_ptr<ChainedSearchStopper> SelfPlayLimits::MakeSearchStopper()
    const {
  auto result = std::make_unique<ChainedSearchStopper>();

  if (visits >= 0) result->AddStopper(std::make_unique<VisitsStopper>(visits));
  if (playouts >= 0) {
    result->AddStopper(std::make_unique<PlayoutsStopper>(playouts));
  }
  if (movetime >= 0) {
    result->AddStopper(std::make_unique<TimeLimitStopper>(movetime));
  }
  return result;
}

}  // namespace lczero
