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

struct LineEvent {
  unsigned col;
  unsigned len;
  string expansion;
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

bool getLineOffset(const string &filename, unsigned lineNum, unsigned &offset) {
  ifstream ifs(filename);
  if (!ifs)
    return false;
  offset = 0;
  string line;
  unsigned current = 0;
  while (getline(ifs, line)) {
    current++;
    if (current == lineNum)
      return true;
    offset += line.size() + 1; // 改行を1文字としてカウント
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
    if (MD.getMacroInfo() && MD.getMacroInfo()->isFunctionLike())
      origText = getFullMacroInvocationText(SM, MacroNameTok.getLocation(), LangOpts);
    else {
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
      SourceLocation origEndLoc = Lexer::getLocForEndOfToken(Range.getEnd(), 0, SM, LangOpts);
      ev.originalLength = SM.getFileOffset(origEndLoc) - ev.originalOffset;
    } else {
      ev.originalOffset = 0;
      ev.originalLength = 0;
    }
    gMacroEvents.push_back(ev);
    // （DEBUG 表示は省略可）
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
// ここで、Clang 18 の public API StringifyArgument を利用して実引数の文字列化を行う。
// Signature: StringifyArgument(const Token *ArgToks, Preprocessor &PP, bool Charify,
//                              SourceLocation ExpansionLocStart, SourceLocation ExpansionLocEnd)
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
          // StringifyArgument を使って実引数の文字列を取得
          // ExpansionLocStart/End は実引数の最初のトークンから算出する
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
  // 余分な空白があれば llvm::StringRef::trim() で除去
  //result = StringRef(result).trim().str();
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

string replaceMacrosInLine(const string &line, unsigned lineOffset, const vector<MacroEvent>& events) {
    string newLine = line;
    vector<LineEvent> lineEvents;

    // 展開されたマクロのイベントを処理
    for (const auto &ev : events) {
        if (ev.originalOffset >= lineOffset && ev.originalOffset < lineOffset + line.size()) {
            unsigned col = ev.originalOffset - lineOffset;
            lineEvents.push_back({col, ev.originalLength, ev.expansionText});
        }
    }

    // イベントをソートして、後ろから前に向かって置き換えを行う
    std::sort(lineEvents.begin(), lineEvents.end(), [](const LineEvent &a, const LineEvent &b) {
        return a.col > b.col;
    });

    for (const auto &le : lineEvents) {
        if (le.col + le.len <= newLine.size()) {
            string expanded = le.expansion;
            size_t pos = 0;
            // 元のソースに空白を挿入して置き換え
            newLine.replace(le.col, le.len, expanded);
        }
    }

    return newLine;
}

// InMemoryFileSystem を使ってファイルをメモリ上に展開するユーティリティ
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
  // targetFileName は、メインファイルの名前として使用
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
  string currentLine = lines[gTargetLine - 1];
  outs() << "\nInitial target line: " << currentLine << "\n";

  bool changed = true;
  int iteration = 0;
  const int maxIteration = 10;

  while (changed && iteration < maxIteration) {
    outs() << "\n--- Iteration " << iteration << " ---\n";
    lines[gTargetLine - 1] = currentLine;
    string updatedContents = joinLines(lines);
    auto MemFS = createInMemoryFSWithFile(targetFileName, updatedContents);

    // OverlayFileSystem を利用して、実際のファイルシステムと重ね合わせる
    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> OverlayFS(new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
    OverlayFS->pushOverlay(MemFS);

    gMacroEvents.clear();
    gRecordTopLevelOnly = true;
    ClangTool Tool(Compilations, OptionsParser.getSourcePathList());
    Tool.getFiles().setVirtualFileSystem(OverlayFS);
    Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(CLA, ArgumentInsertPosition::BEGIN));

    int result = Tool.run(newFrontendActionFactory<MacroExpansionAction>().get());
    (void)result;

    unsigned lineOffset = 0;
    if (!getLineOffset(targetFileName, gTargetLine, lineOffset)) {
      errs() << "Failed to get line offset from file!\n";
      return 1;
    }

    string newLine = replaceMacrosInLine(currentLine, lineOffset, gMacroEvents);
    if (newLine == currentLine)
      changed = false;
    else {
      outs() << "Iteration " << iteration << " result: " << newLine << "\n";
      currentLine = newLine;
      changed = true;
    }
    iteration++;
  }

  {
    lines[gTargetLine - 1] = currentLine;
    string updatedContents = joinLines(lines);
    auto MemFS = createInMemoryFSWithFile(targetFileName, updatedContents);
    IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> OverlayFS(new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
    OverlayFS->pushOverlay(MemFS);
    gMacroEvents.clear();
    gRecordTopLevelOnly = false;
    ClangTool Tool(Compilations, OptionsParser.getSourcePathList());
    Tool.getFiles().setVirtualFileSystem(OverlayFS);
    Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(CLA, ArgumentInsertPosition::BEGIN));
    int result = Tool.run(newFrontendActionFactory<MacroExpansionAction>().get());
    (void)result;
    unsigned lineOffset = 0;
    if (!getLineOffset(targetFileName, gTargetLine, lineOffset)) {
      errs() << "Failed to get line offset from file (Pass2)!\n";
      return 1;
    }
    string finalLine = replaceMacrosInLine(currentLine, lineOffset, gMacroEvents);
    outs() << "\nFinal expanded line: " << finalLine << "\n";
  }

  return 0;
}


