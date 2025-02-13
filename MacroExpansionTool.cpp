// MacroExpansionTool.cpp

#include "clang/Basic/LangOptions.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/CompilerInstance.h"
// PreprocessOnlyAction は FrontendActions.h 内に定義されています
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroArgs.h" // MacroArgs の完全定義のため
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Tooling/ArgumentsAdjusters.h"
#include "clang/Tooling/CompilationDatabase.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>
#include <regex>
#include <memory>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;
using namespace std;

//===----------------------------------------------------------------------===//
// グローバル変数とデータ構造
//===----------------------------------------------------------------------===//

struct MacroEvent {
  unsigned level;         // 0:トップレベル、1:入れ子など
  unsigned originalOffset;  // （トップレベル用）元のファイル内でのオフセット
  unsigned originalLength;  // （トップレベル用）
  string macroName;
  string originalText;  // マクロ呼び出し時のテキスト
  string expansionText; // 展開結果のテキスト
};

static vector<MacroEvent> gMacroEvents;
static cl::opt<unsigned> gTargetLine("line", cl::desc("Target line number (1-indexed)"), cl::Required);
static cl::OptionCategory MacroToolCategory("macro-expansion-tool options");

static bool gRecordTopLevelOnly = true;

//===----------------------------------------------------------------------===//
// ヘルパー関数
//===----------------------------------------------------------------------===//

unsigned computeNestingLevel(SourceManager &SM, SourceLocation Loc) {
  unsigned level = 0;
  while (Loc.isMacroID()) {
    level++;
    Loc = SM.getImmediateSpellingLoc(Loc);
  }
  return level;
}

bool getLineFromFile(const string &filename, unsigned lineNum, string &outLine) {
  ifstream ifs(filename);
  if (!ifs)
    return false;
  string line;
  unsigned current = 0;
  while (getline(ifs, line)) {
    current++;
    if (current == lineNum) {
      outLine = line;
      return true;
    }
  }
  return false;
}

string readFileContents(const string &filename) {
  ifstream ifs(filename);
  ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

vector<string> splitIntoLines(const string &contents) {
  vector<string> lines;
  istringstream iss(contents);
  string line;
  while (getline(iss, line))
    lines.push_back(line);
  return lines;
}

string joinLines(const vector<string> &lines) {
  string result;
  for (const auto &line : lines)
    result += line + "\n";
  return result;
}

class MacroExpansionPrinter : public PPCallbacks {
public:
  MacroExpansionPrinter(CompilerInstance &CI, unsigned TargetLine)
    : SM(CI.getSourceManager()),
      PP(CI.getPreprocessor()),
      TargetFile(SM.getFilename(SM.getLocForStartOfFile(SM.getMainFileID())).str()),
      TargetLine(TargetLine), printedOriginal(false) {}

  void MacroExpands(const Token &MacroNameTok,
                    const MacroDefinition &MD,
                    SourceRange Range,
                    const MacroArgs *Args) override {
    SourceLocation expansionLoc = SM.getExpansionLoc(MacroNameTok.getLocation());
    StringRef file = SM.getFilename(expansionLoc);
    unsigned line = SM.getExpansionLineNumber(expansionLoc);
    if (file != TargetFile || line != TargetLine)
      return;
    unsigned level = computeNestingLevel(SM, MacroNameTok.getLocation());
    if (gRecordTopLevelOnly && level != 0)
      return;
    LangOptions LangOpts;
    string origText;
    if (MD.getMacroInfo() && MD.getMacroInfo()->isFunctionLike()) {
      origText = getFullMacroInvocationText(SM, MacroNameTok.getLocation(), LangOpts);
      string macroName = MacroNameTok.getIdentifierInfo()->getName().str();
      // 既に展開済みなら、このイベントはスキップする
      if (origText.find(macroName) != 0)
        return;
    } else {
      SourceLocation origBegin = Range.getBegin();
      SourceLocation origEnd = Lexer::getLocForEndOfToken(Range.getEnd(), 0, SM, LangOpts);
      origText = string(Lexer::getSourceText(CharSourceRange::getTokenRange(origBegin, origEnd), SM, LangOpts));
    }
    string computedExpText;
    if (MD.getMacroInfo() && MD.getMacroInfo()->isFunctionLike() && Args)
      computedExpText = computeMacroReplacement(MD, Args, SM, LangOpts, PP);
    else {
      SourceLocation expBegin = SM.getExpansionLoc(Range.getBegin());
      SourceLocation expEnd = Lexer::getLocForEndOfToken(SM.getExpansionLoc(Range.getEnd()), 0, SM, LangOpts);
      computedExpText = string(Lexer::getSourceText(CharSourceRange::getTokenRange(expBegin, expEnd), SM, LangOpts));
    }
    MacroEvent ev;
    ev.level = level;
    ev.macroName = MacroNameTok.getIdentifierInfo()->getName().str();
    ev.originalText = origText;
    ev.expansionText = computedExpText;
    if (level == 0) {
      ev.originalOffset = SM.getFileOffset(Range.getBegin());
      if (MD.getMacroInfo() && MD.getMacroInfo()->isFunctionLike())
        ev.originalLength = origText.size();
      else {
        SourceLocation origEndLoc = Lexer::getLocForEndOfToken(Range.getEnd(), 0, SM, LangOpts);
        ev.originalLength = SM.getFileOffset(origEndLoc) - ev.originalOffset;
      }
    } else {
      ev.originalOffset = 0;
      ev.originalLength = 0;
    }
    gMacroEvents.push_back(ev);
  }

private:
  SourceManager &SM;
  Preprocessor &PP;
  string TargetFile;
  unsigned TargetLine;
  bool printedOriginal;

  string getFullMacroInvocationText(SourceManager &SM, SourceLocation MacroNameLoc, const LangOptions &LangOpts) {
    SourceLocation startLoc = SM.getSpellingLoc(MacroNameLoc);
    bool invalid = false;
    StringRef buffer = SM.getBufferData(SM.getFileID(startLoc), &invalid);
    if (invalid)
      return "";
    unsigned offset = SM.getFileOffset(startLoc);
    bool foundParen = false;
    int parenCount = 0;
    unsigned endOffset = offset;
    for (unsigned i = offset, e = buffer.size(); i < e; i++) {
      char c = buffer[i];
      if (!foundParen) {
        if (c == '(') {
          foundParen = true;
          parenCount = 1;
        }
      } else {
        if (c == '(')
          parenCount++;
        else if (c == ')') {
          parenCount--;
          if (parenCount == 0) {
            endOffset = i + 1;
            break;
          }
        }
      }
    }
    return string(buffer.substr(offset, endOffset - offset));
  }

  string computeMacroReplacement(const MacroDefinition &MD, const MacroArgs *Args,
                                    SourceManager &SM, const LangOptions &LangOpts,
                                    Preprocessor &PP) {
    const MacroInfo *MI = MD.getMacroInfo();
    if (!MI)
      return "";
    string result;
    if (!MI->isFunctionLike()) {
      for (const auto &Tok : MI->tokens()) {
        result += Lexer::getSpelling(Tok, SM, LangOpts);
      }
      return result;
    }
    auto &firsttok = MI->tokens()[0];
    int processed = SM.getExpansionColumnNumber(SM.getExpansionLoc(firsttok.getLocation()));
    for (const auto &Tok : MI->tokens()) {
      if (Tok.is(tok::identifier)) {
        IdentifierInfo *II = Tok.getIdentifierInfo();
        bool substituted = false;
        for (auto It = MI->param_begin(), E = MI->param_end(); It != E; ++It) {
          IdentifierInfo *ParamII = *It;
          if (ParamII && (ParamII->getName() == II->getName())) {
            unsigned paramIndex = static_cast<unsigned>(It - MI->param_begin());
            const Token *ArgToks = Args->getUnexpArgument(paramIndex);
            if (ArgToks) {
              SourceLocation startLoc = SM.getExpansionLoc(ArgToks->getLocation());
              SourceLocation endLoc = Lexer::getLocForEndOfToken(ArgToks->getLocation(), 0, SM, LangOpts);
              string argStr = Lexer::getSourceText(CharSourceRange::getCharRange(startLoc, endLoc), SM, LangOpts).str();
              processed += (SM.getExpansionColumnNumber(endLoc) - SM.getExpansionColumnNumber(startLoc));
              result += argStr;
              substituted = true;
            }
            break;
          }
        }
        if (!substituted) {
          SourceLocation startLoc = SM.getExpansionLoc(Tok.getLocation());
          SourceLocation endLoc = Lexer::getLocForEndOfToken(Tok.getLocation(), 0, SM, LangOpts);
          for (; processed < SM.getExpansionColumnNumber(startLoc); processed++) {
            result += " ";
          }
          processed += (SM.getExpansionColumnNumber(endLoc) - SM.getExpansionColumnNumber(startLoc));
          result += Lexer::getSpelling(Tok, SM, LangOpts);
        }
      } else {
        SourceLocation startLoc = SM.getExpansionLoc(Tok.getLocation());
        SourceLocation endLoc = Lexer::getLocForEndOfToken(Tok.getLocation(), 0, SM, LangOpts);
        for (; processed < SM.getExpansionColumnNumber(startLoc); processed++) {
          result += " ";
        }
        processed += (SM.getExpansionColumnNumber(endLoc) - SM.getExpansionColumnNumber(startLoc));
        result += Lexer::getSpelling(Tok, SM, LangOpts);
      }
    }
    return result;
  }
};

class MacroExpansionAction : public PreprocessOnlyAction {
protected:
  void ExecuteAction() override {
    CompilerInstance &CI = getCompilerInstance();
    CI.getPreprocessor().addPPCallbacks(
      make_unique<MacroExpansionPrinter>(CI, gTargetLine)
    );
    PreprocessOnlyAction::ExecuteAction();
  }
};

// 置換処理について
// ここでは、元のテキスト（text）全体を先頭から順に走査し、事前に収集した置換イベント（replacements）
// に基づいて、新しい文字列（result）を構築します。
// 各 Replacement は、以下の情報を持ちます:
//   pos: 元のテキスト内の開始位置（基準は baseOffset で補正済み）
//   len: 置換対象の文字数
//   expansion: 置換後に挿入すべき文字列
//   origText: 置換対象として期待される原文（これと実際の部分文字列が一致する場合のみ置換を適用）
string replaceMacrosInLine(const string &text, unsigned baseOffset, const vector<MacroEvent>& events) {
    struct Replacement {
      unsigned pos;
      unsigned len;
      string expansion;
      string origText;
    };
    vector<Replacement> replacements;
    for (const auto &ev : events) {
        if (ev.originalLength == 0)
            continue;
        if (ev.originalOffset >= baseOffset && ev.originalOffset < baseOffset + text.size()) {
            unsigned relPos = ev.originalOffset - baseOffset;
            replacements.push_back({relPos, ev.originalLength, ev.expansionText, ev.originalText});
        }
    }
    std::sort(replacements.begin(), replacements.end(), [](const Replacement &a, const Replacement &b) {
        return a.pos < b.pos;
    });
    
    string result;
    size_t pos = 0;
    for (const auto &rep : replacements) {
        // まず、置換対象部分が、期待される原文と一致するかをチェックする
        if (rep.pos + rep.len <= text.size() && text.substr(rep.pos, rep.len) == rep.origText) {
            if (rep.pos > pos)
                result.append(text, pos, rep.pos - pos);
            result.append(rep.expansion);
            pos = rep.pos + rep.len;
        }
    }
    if (pos < text.size())
        result.append(text, pos, text.size() - pos);
    return result;
}

// InMemoryFileSystem を使って、ファイル内容をメモリ上に展開する
IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> createInMemoryFSWithFile(const string &filename,
                                                                           const string &contents) {
  IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem> InMemFS(new llvm::vfs::InMemoryFileSystem);
  InMemFS->addFile(filename, 0, MemoryBuffer::getMemBuffer(contents, filename));
  return InMemFS;
}

int main(int argc, const char **argv) {
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, MacroToolCategory);
  if (!ExpectedParser) {
    auto Err = ExpectedParser.takeError();
    errs() << "Error creating CommonOptionsParser: " << toString(std::move(Err)) << "\n";
    return 1;
  }
  CommonOptionsParser &OptionsParser = *ExpectedParser;
  auto SourceFiles = OptionsParser.getSourcePathList();
  if (SourceFiles.empty()) {
    errs() << "No source file specified!\n";
    return 1;
  }
  string targetFileName = SourceFiles[0];

  vector<string> ExtraArgs = {"-Xclang", "-detailed-preprocessing-record"};
  CommandLineArguments CLA(ExtraArgs.begin(), ExtraArgs.end());
  FixedCompilationDatabase Compilations(".", vector<string>());

  string fileContents = readFileContents(targetFileName);
  vector<string> lines = splitIntoLines(fileContents);
  if (gTargetLine - 1 >= lines.size()) {
    errs() << "Target line number is out of range.\n";
    return 1;
  }
  // 複数行にまたがる文の場合、末尾が ';' または '}' の行までを結合
  unsigned stmtStart = gTargetLine - 1;
  unsigned stmtEnd = stmtStart;
  while (stmtEnd < lines.size()) {
    string trimmed = lines[stmtEnd];
    size_t endpos = trimmed.find_last_not_of(" \t\r\n");
    if (endpos != string::npos)
      trimmed = trimmed.substr(0, endpos + 1);
    if (!trimmed.empty() && (trimmed.back() == ';' || trimmed.back() == '}'))
      break;
    stmtEnd++;
  }
  string currentStmt;
  for (unsigned i = stmtStart; i <= stmtEnd && i < lines.size(); i++) {
    currentStmt += lines[i];
    if (i < stmtEnd)
      currentStmt += "\n";
  }
  outs() << "\nInitial target statement: " << currentStmt << "\n";

  bool changed = true;
  int iteration = 0;
  const int maxIteration = 10;

  while (changed && iteration < maxIteration) {
    outs() << "\n--- Iteration " << iteration << " ---\n";
    // ファイル内容の更新：対象文を currentStmt に置き換える
    lines.erase(lines.begin() + stmtStart, lines.begin() + stmtEnd + 1);
    lines.insert(lines.begin() + stmtStart, currentStmt);
    string updatedContents = joinLines(lines);
    auto MemFS = createInMemoryFSWithFile(targetFileName, updatedContents);
    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> OverlayFS(
         new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
    OverlayFS->pushOverlay(MemFS);

    gMacroEvents.clear();
    gRecordTopLevelOnly = true;
    ClangTool Tool(Compilations, OptionsParser.getSourcePathList());
    Tool.getFiles().setVirtualFileSystem(OverlayFS);
    Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(CLA, ArgumentInsertPosition::BEGIN));
    int result = Tool.run(newFrontendActionFactory<MacroExpansionAction>().get());
    (void)result;

    // 更新後のファイルから、対象文の先頭までのオフセットを計算
    unsigned stmtOffset = 0;
    for (unsigned i = 0; i < stmtStart; i++) {
      stmtOffset += lines[i].size() + 1;
    }
    string newStmt = replaceMacrosInLine(currentStmt, stmtOffset, gMacroEvents);
    if (newStmt == currentStmt)
      changed = false;
    else {
      outs() << "Iteration " << iteration << " result: " << newStmt << "\n";
      currentStmt = newStmt;
      changed = true;
    }
    iteration++;
    stmtEnd = stmtStart;
  }

  {
    lines.erase(lines.begin() + stmtStart, lines.begin() + stmtEnd + 1);
    lines.insert(lines.begin() + stmtStart, currentStmt);
    string updatedContents = joinLines(lines);
    auto MemFS = createInMemoryFSWithFile(targetFileName, updatedContents);
    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> OverlayFS(
         new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
    OverlayFS->pushOverlay(MemFS);
    gMacroEvents.clear();
    gRecordTopLevelOnly = false;
    ClangTool Tool(Compilations, OptionsParser.getSourcePathList());
    Tool.getFiles().setVirtualFileSystem(OverlayFS);
    Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(CLA, ArgumentInsertPosition::BEGIN));
    int result = Tool.run(newFrontendActionFactory<MacroExpansionAction>().get());
    (void)result;
    unsigned stmtOffset = 0;
    for (unsigned i = 0; i < stmtStart; i++) {
      stmtOffset += lines[i].size() + 1;
    }
    string finalStmt = replaceMacrosInLine(currentStmt, stmtOffset, gMacroEvents);
    outs() << "\nFinal expanded statement: " << finalStmt << "\n";
  }

  return 0;
}

