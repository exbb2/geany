/******************************************************************
 *    LexHaskell.cxx
 *
 *    A haskell lexer for the scintilla code control.
 *    Some stuff "lended" from LexPython.cxx and LexCPP.cxx.
 *    External lexer stuff inspired from the caml external lexer.
 *    Folder copied from Python's.
 *
 *    Written by Tobias Engvall - tumm at dtek dot chalmers dot se
 *
 *    Several bug fixes by Krasimir Angelov - kr.angelov at gmail.com
 *
 *    Improved by kudah <kudahkukarek@gmail.com>
 *
 *    TODO:
 *    * A proper lexical folder to fold group declarations, comments, pragmas,
 *      #ifdefs, explicit layout, lists, tuples, quasi-quotes, splces, etc, etc,
 *      etc.
 *
 *****************************************************************/
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>
#include <ctype.h>

#include <string>
#include <map>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "PropSetSimple.h"
#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
#include "CharacterSet.h"
#include "LexerModule.h"
#include "OptionSet.h"

#ifdef SCI_NAMESPACE
using namespace Scintilla;
#endif

#define HA_MODE_DEFAULT     0
#define HA_MODE_IMPORT1     1
#define HA_MODE_IMPORT2     2
#define HA_MODE_IMPORT3     3
#define HA_MODE_MODULE      4
#define HA_MODE_FFI         5
#define HA_MODE_TYPE        6
#define HA_MODE_PRAGMA      7

#define INDENT_OFFSET       1

static int u_iswalpha(int);
static int u_iswalnum(int);
static int u_iswupper(int);
static int u_IsHaskellSymbol(int);

#define HASKELL_UNICODE

#ifndef HASKELL_UNICODE

// Stubs

static int u_iswalpha(int) {
   return 0;
}

static int u_iswalnum(int) {
   return 0;
}

static int u_iswupper(int) {
   return 0;
}

static int u_IsHaskellSymbol(int) {
   return 0;
}

#endif

static inline bool IsHaskellLetter(const int ch) {
   if (IsASCII(ch)) {
      return (ch >= 'a' && ch <= 'z')
          || (ch >= 'A' && ch <= 'Z');
   } else {
      return u_iswalpha(ch) != 0;
   }
}

static inline bool IsHaskellAlphaNumeric(const int ch) {
   if (IsASCII(ch)) {
      return IsAlphaNumeric(ch);
   } else {
      return u_iswalnum(ch) != 0;
   }
}

static inline bool IsHaskellUpperCase(const int ch) {
   if (IsASCII(ch)) {
      return ch >= 'A' && ch <= 'Z';
   } else {
      return u_iswupper(ch) != 0;
   }
}

static inline bool IsAnHaskellOperatorChar(const int ch) {
   if (IsASCII(ch)) {
      return
         (  ch == '!' || ch == '#' || ch == '$' || ch == '%'
         || ch == '&' || ch == '*' || ch == '+' || ch == '-'
         || ch == '.' || ch == '/' || ch == ':' || ch == '<'
         || ch == '=' || ch == '>' || ch == '?' || ch == '@'
         || ch == '^' || ch == '|' || ch == '~' || ch == '\\');
   } else {
      return u_IsHaskellSymbol(ch) != 0;
   }
}

static inline bool IsAHaskellWordStart(const int ch) {
   return IsHaskellLetter(ch) || ch == '_';
}

static inline bool IsAHaskellWordChar(const int ch) {
   return (  IsHaskellAlphaNumeric(ch)
          || ch == '_'
          || ch == '\'');
}

static inline bool IsCommentBlockStyle(int style) {
   return (style >= SCE_HA_COMMENTBLOCK && style <= SCE_HA_COMMENTBLOCK3);
}

static inline bool IsCommentStyle(int style) {
   return (style >= SCE_HA_COMMENTLINE && style <= SCE_HA_COMMENTBLOCK3);
}

inline int StyleFromNestLevel(const unsigned int nestLevel) {
      return SCE_HA_COMMENTBLOCK + (nestLevel % 3);
   }

struct OptionsHaskell {
   bool magicHash;
   bool allowQuotes;
   bool implicitParams;
   bool highlightSafe;
   bool stylingWithinPreprocessor;
   bool fold;
   bool foldComment;
   bool foldCompact;
   bool foldImports;
   bool foldIndentedImports;
   OptionsHaskell() {
      magicHash = true;       // Widespread use, enabled by default.
      allowQuotes = true;     // Widespread use, enabled by default.
      implicitParams = false; // Fell out of favor, seldom used, disabled.
      highlightSafe = true;   // Moderately used, doesn't hurt to enable.
      stylingWithinPreprocessor = false;
      fold = false;
      foldComment = false;
      foldCompact = false;
      foldImports = false;
      foldIndentedImports = true;
   }
};

static const char * const haskellWordListDesc[] = {
   "Keywords",
   "FFI",
   0
};

struct OptionSetHaskell : public OptionSet<OptionsHaskell> {
   OptionSetHaskell() {
      DefineProperty("lexer.haskell.allow.hash", &OptionsHaskell::magicHash,
         "Set to 0 to disallow the '#' character at the end of identifiers and "
         "literals with the haskell lexer "
         "(GHC -XMagicHash extension)");

      DefineProperty("lexer.haskell.allow.quotes", &OptionsHaskell::allowQuotes,
         "Set to 0 to disable highlighting of Template Haskell name quotations "
         "and promoted constructors "
         "(GHC -XTemplateHaskell and -XDataKinds extensions)");

      DefineProperty("lexer.haskell.allow.questionmark", &OptionsHaskell::implicitParams,
         "Set to 1 to allow the '?' character at the start of identifiers "
         "with the haskell lexer "
         "(GHC & Hugs -XImplicitParams extension)");

      DefineProperty("lexer.haskell.import.safe", &OptionsHaskell::highlightSafe,
         "Set to 0 to disallow \"safe\" keyword in imports "
         "(GHC -XSafe, -XTrustworthy, -XUnsafe extensions)");

      DefineProperty("styling.within.preprocessor", &OptionsHaskell::stylingWithinPreprocessor,
         "For Haskell code, determines whether all preprocessor code is styled in the "
         "preprocessor style (0, the default) or only from the initial # to the end "
         "of the command word(1)."
         );

      DefineProperty("fold", &OptionsHaskell::fold);

      DefineProperty("fold.comment", &OptionsHaskell::foldComment);

      DefineProperty("fold.compact", &OptionsHaskell::foldCompact);

      DefineProperty("fold.haskell.imports", &OptionsHaskell::foldImports,
         "Set to 1 to enable folding of import declarations");

      DefineProperty("fold.haskell.imports.indented", &OptionsHaskell::foldIndentedImports,
         "Set this property to 0 to disable folding imports not starting at "
         "column 0 when fold.haskell.imports=1");

      DefineWordListSets(haskellWordListDesc);
   }
};

class LexerHaskell : public ILexer {
   int firstImportLine;
   WordList keywords;
   WordList ffi;
   WordList reserved_operators;
   OptionsHaskell options;
   OptionSetHaskell osHaskell;

   enum HashCount {
       oneHash
      ,twoHashes
      ,unlimitedHashes
   };

   inline void skipMagicHash(StyleContext &sc, const HashCount hashes) {
      if (options.magicHash && sc.ch == '#') {
         sc.Forward();
         if (hashes == twoHashes && sc.ch == '#') {
            sc.Forward();
         } else if (hashes == unlimitedHashes) {
            while (sc.ch == '#') {
               sc.Forward();
            }
         }
      }
   }

   bool LineContainsImport(const int line, Accessor &styler) {
      if (options.foldImports) {
         int currentPos = styler.LineStart(line);
         int style = styler.StyleAt(currentPos);

         if (options.foldIndentedImports) {
            int eol_pos = styler.LineStart(line + 1) - 1;

            while (currentPos < eol_pos) {
               int ch = styler[currentPos];
               style = styler.StyleAt(currentPos);

               if (ch == ' ' || ch == '\t'
                || IsCommentBlockStyle(style)) {
                  currentPos++;
               } else {
                  break;
               }
            }
         }

         return (style == SCE_HA_KEYWORD
              && styler.Match(currentPos, "import"));
      } else {
         return false;
      }
   }
public:
   LexerHaskell() : firstImportLine(-1) {}
   virtual ~LexerHaskell() {}

   void SCI_METHOD Release() {
      delete this;
   }

   int SCI_METHOD Version() const {
      return lvOriginal;
   }

   const char * SCI_METHOD PropertyNames() {
      return osHaskell.PropertyNames();
   }

   int SCI_METHOD PropertyType(const char *name) {
      return osHaskell.PropertyType(name);
   }

   const char * SCI_METHOD DescribeProperty(const char *name) {
      return osHaskell.DescribeProperty(name);
   }

   int SCI_METHOD PropertySet(const char *key, const char *val);

   const char * SCI_METHOD DescribeWordListSets() {
      return osHaskell.DescribeWordListSets();
   }

   int SCI_METHOD WordListSet(int n, const char *wl);

   void SCI_METHOD Lex(unsigned int startPos, int length, int initStyle, IDocument *pAccess);

   void SCI_METHOD Fold(unsigned int startPos, int length, int initStyle, IDocument *pAccess);

   void * SCI_METHOD PrivateCall(int, void *) {
      return 0;
   }

   static ILexer *LexerFactoryHaskell() {
      return new LexerHaskell();
   }
};

int SCI_METHOD LexerHaskell::PropertySet(const char *key, const char *val) {
   if (osHaskell.PropertySet(&options, key, val)) {
      return 0;
   }
   return -1;
}

int SCI_METHOD LexerHaskell::WordListSet(int n, const char *wl) {
   WordList *wordListN = 0;
   switch (n) {
   case 0:
      wordListN = &keywords;
      break;
   case 1:
      wordListN = &ffi;
      break;
   case 2:
      wordListN = &reserved_operators;
      break;
   }
   int firstModification = -1;
   if (wordListN) {
      WordList wlNew;
      wlNew.Set(wl);
      if (*wordListN != wlNew) {
         wordListN->Set(wl);
         firstModification = 0;
      }
   }
   return firstModification;
}

void SCI_METHOD LexerHaskell::Lex(unsigned int startPos, int length, int initStyle
                                 ,IDocument *pAccess) {
   LexAccessor styler(pAccess);

   // Do not leak onto next line
   if (initStyle == SCE_HA_STRINGEOL)
      initStyle = SCE_HA_DEFAULT;

   StyleContext sc(startPos, length, initStyle, styler);

   int lineCurrent = styler.GetLine(startPos);

   int state = lineCurrent ? styler.GetLineState(lineCurrent-1) : 0;
   int mode  = state & 0x7;
   int nestLevel = state >> 3;

   int base = 10;
   bool inDashes = false;

   assert(!(IsCommentBlockStyle(initStyle) && nestLevel <= 0));

   while (sc.More()) {
      // Check for state end

      // For line numbering (and by extension, nested comments) to work,
      // states should always forward one character at a time.
      // states should not query atLineEnd, use sc.ch == '\n' || sc.ch == '\r'
      // instead.
      // If a state sometimes does _not_ forward a character, it should check
      // first if it's not on a line end and forward otherwise.
      // If a state forwards more than one character, it should check every time
      // that it is not a line end and cease forwarding otherwise.
      if (sc.atLineEnd) {
         // Remember the line state for future incremental lexing
         styler.SetLineState(lineCurrent, (nestLevel << 3) | mode);
         lineCurrent++;
      }

      if (sc.atLineStart && (sc.state == SCE_HA_STRING || sc.state == SCE_HA_CHARACTER)) {
         // Prevent SCE_HA_STRINGEOL from leaking back to previous line
         sc.SetState(sc.state);
      }

      // Handle line continuation generically.
      if (sc.ch == '\\' &&
         (  sc.state == SCE_HA_STRING
         || sc.state == SCE_HA_PREPROCESSOR)) {
         if (sc.chNext == '\n' || sc.chNext == '\r') {
            sc.Forward();

            // Remember the line state for future incremental lexing
            styler.SetLineState(lineCurrent, (nestLevel << 3) | mode);
            lineCurrent++;

            if (sc.ch == '\r' && sc.chNext == '\n') {
               sc.Forward();
            }
            sc.Forward();
            continue;
         }
      }

         // Operator
      if (sc.state == SCE_HA_OPERATOR) {
         int style = SCE_HA_OPERATOR;

         if (sc.ch == ':' &&
            // except "::"
            !(sc.chNext == ':' && !IsAnHaskellOperatorChar(sc.GetRelative(2)))) {
            style = SCE_HA_CAPITAL;
         }

         while (IsAnHaskellOperatorChar(sc.ch))
               sc.Forward();

         char s[100];
         sc.GetCurrent(s, sizeof(s));

         if (reserved_operators.InList(s))
            style = SCE_HA_RESERVED_OPERATOR;

         sc.ChangeState(style);
         sc.SetState(SCE_HA_DEFAULT);
      }
         // String
      else if (sc.state == SCE_HA_STRING) {
         if (sc.ch == '\n' || sc.ch == '\r') {
            sc.ChangeState(SCE_HA_STRINGEOL);
            sc.ForwardSetState(SCE_HA_DEFAULT);
         } else if (sc.ch == '\"') {
            sc.Forward();
            skipMagicHash(sc, oneHash);
            sc.SetState(SCE_HA_DEFAULT);
         } else if (sc.ch == '\\') {
            sc.Forward(2);
         } else {
            sc.Forward();
         }
      }
         // Char
      else if (sc.state == SCE_HA_CHARACTER) {
         if (sc.ch == '\n' || sc.ch == '\r') {
            sc.ChangeState(SCE_HA_STRINGEOL);
            sc.ForwardSetState(SCE_HA_DEFAULT);
         } else if (sc.ch == '\'') {
            sc.Forward();
            skipMagicHash(sc, oneHash);
            sc.SetState(SCE_HA_DEFAULT);
         } else if (sc.ch == '\\') {
            sc.Forward(2);
         } else {
            sc.Forward();
         }
      }
         // Number
      else if (sc.state == SCE_HA_NUMBER) {
         if (IsADigit(sc.ch, base) ||
            (sc.ch=='.' && IsADigit(sc.chNext, base))) {
            sc.Forward();
         } else if ((base == 10) &&
                    (sc.ch == 'e' || sc.ch == 'E') &&
                    (IsADigit(sc.chNext) || sc.chNext == '+' || sc.chNext == '-')) {
            sc.Forward();
            if (sc.ch == '+' || sc.ch == '-')
                sc.Forward();
         } else {
            skipMagicHash(sc, twoHashes);
            sc.SetState(SCE_HA_DEFAULT);
         }
      }
         // Keyword or Identifier
      else if (sc.state == SCE_HA_IDENTIFIER) {
         int style = IsHaskellUpperCase(sc.ch) ? SCE_HA_CAPITAL : SCE_HA_IDENTIFIER;

         assert(IsAHaskellWordStart(sc.ch));

         sc.Forward();

         while (sc.More()) {
            if (IsAHaskellWordChar(sc.ch)) {
               sc.Forward();
            } else if (sc.ch == '.' && style == SCE_HA_CAPITAL) {
               if (IsHaskellUpperCase(sc.chNext)) {
                  sc.Forward();
                  style = SCE_HA_CAPITAL;
               } else if (IsAHaskellWordStart(sc.chNext)) {
                  sc.Forward();
                  style = SCE_HA_IDENTIFIER;
               } else if (IsAnHaskellOperatorChar(sc.chNext)) {
                  sc.Forward();
                  style = sc.ch == ':' ? SCE_HA_CAPITAL : SCE_HA_OPERATOR;
                  while (IsAnHaskellOperatorChar(sc.ch))
                     sc.Forward();
                  break;
               } else {
                  break;
               }
            } else {
               break;
            }
         }

         skipMagicHash(sc, unlimitedHashes);

         char s[100];
         sc.GetCurrent(s, sizeof(s));

         int new_mode = HA_MODE_DEFAULT;

         if (keywords.InList(s)) {
            style = SCE_HA_KEYWORD;
         } else if (style == SCE_HA_CAPITAL) {
            if (mode == HA_MODE_IMPORT1 || mode == HA_MODE_IMPORT3) {
               style    = SCE_HA_MODULE;
               new_mode = HA_MODE_IMPORT2;
            } else if (mode == HA_MODE_MODULE) {
               style = SCE_HA_MODULE;
            }
         } else if (mode == HA_MODE_IMPORT1 &&
                    strcmp(s,"qualified") == 0) {
             style    = SCE_HA_KEYWORD;
             new_mode = HA_MODE_IMPORT1;
         } else if (options.highlightSafe &&
                    mode == HA_MODE_IMPORT1 &&
                    strcmp(s,"safe") == 0) {
             style    = SCE_HA_KEYWORD;
             new_mode = HA_MODE_IMPORT1;
         } else if (mode == HA_MODE_IMPORT2) {
             if (strcmp(s,"as") == 0) {
                style    = SCE_HA_KEYWORD;
                new_mode = HA_MODE_IMPORT3;
            } else if (strcmp(s,"hiding") == 0) {
                style     = SCE_HA_KEYWORD;
            }
         } else if (mode == HA_MODE_TYPE) {
            if (strcmp(s,"family") == 0)
               style    = SCE_HA_KEYWORD;
         }

         if (mode == HA_MODE_FFI) {
            if (ffi.InList(s)) {
               style = SCE_HA_KEYWORD;
               new_mode = HA_MODE_FFI;
            }
         }

         sc.ChangeState(style);
         sc.SetState(SCE_HA_DEFAULT);

         if (strcmp(s,"import") == 0 && mode != HA_MODE_FFI)
            new_mode = HA_MODE_IMPORT1;
         else if (strcmp(s,"module") == 0)
            new_mode = HA_MODE_MODULE;
         else if (strcmp(s,"foreign") == 0)
            new_mode = HA_MODE_FFI;
         else if (strcmp(s,"type") == 0
               || strcmp(s,"data") == 0)
            new_mode = HA_MODE_TYPE;

         mode = new_mode;
      }

         // Comments
            // Oneliner
      else if (sc.state == SCE_HA_COMMENTLINE) {
         if (sc.ch == '\n' || sc.ch == '\r') {
            sc.SetState(mode == HA_MODE_PRAGMA ? SCE_HA_PRAGMA : SCE_HA_DEFAULT);
            sc.Forward(); // prevent double counting a line
         } else if (inDashes && sc.ch != '-' && mode != HA_MODE_PRAGMA) {
            inDashes = false;
            if (IsAnHaskellOperatorChar(sc.ch))
               sc.ChangeState(SCE_HA_OPERATOR);
         } else {
            sc.Forward();
         }
      }
            // Nested
      else if (IsCommentBlockStyle(sc.state)) {
         if (sc.Match('{','-')) {
            sc.SetState(StyleFromNestLevel(nestLevel));
            sc.Forward(2);
            nestLevel++;
         } else if (sc.Match('-','}')) {
            sc.Forward(2);
            nestLevel--;
            assert(nestLevel >= 0);
            sc.SetState(
               nestLevel <= 0
                  ? (mode == HA_MODE_PRAGMA ? SCE_HA_PRAGMA : SCE_HA_DEFAULT)
                  : StyleFromNestLevel(nestLevel - 1));
         } else {
            sc.Forward();
         }
      }
            // Pragma
      else if (sc.state == SCE_HA_PRAGMA) {
         if (sc.Match("#-}")) {
            mode = HA_MODE_DEFAULT;
            sc.Forward(3);
            sc.SetState(SCE_HA_DEFAULT);
         } else if (sc.Match('-','-')) {
            sc.SetState(SCE_HA_COMMENTLINE);
            sc.Forward(2);
            inDashes = false;
         } else if (sc.Match('{','-')) {
            sc.SetState(StyleFromNestLevel(nestLevel));
            sc.Forward(2);
            nestLevel = 1;
         } else {
            sc.Forward();
         }
      }
            // Preprocessor
      else if (sc.state == SCE_HA_PREPROCESSOR) {
         if (sc.ch == '\n' || sc.ch == '\r') {
            sc.SetState(SCE_HA_DEFAULT);
            sc.Forward(); // prevent double counting a line
         } else if (options.stylingWithinPreprocessor && !IsHaskellLetter(sc.ch)) {
            sc.SetState(SCE_HA_DEFAULT);
         } else {
            sc.Forward();
         }
      }
            // New state?
      else if (sc.state == SCE_HA_DEFAULT) {
         // Digit
         if (IsADigit(sc.ch)) {
            sc.SetState(SCE_HA_NUMBER);
            if (sc.ch == '0' && (sc.chNext == 'X' || sc.chNext == 'x')) {
               // Match anything starting with "0x" or "0X", too
               sc.Forward(2);
               base = 16;
            } else if (sc.ch == '0' && (sc.chNext == 'O' || sc.chNext == 'o')) {
               // Match anything starting with "0o" or "0O", too
               sc.Forward(2);
               base = 8;
            } else {
               sc.Forward();
               base = 10;
            }
            mode = HA_MODE_DEFAULT;
         }
         // Pragma
         else if (sc.Match("{-#")) {
            mode = HA_MODE_PRAGMA;
            sc.SetState(SCE_HA_PRAGMA);
            sc.Forward(3);
         }
         // Comment line
         else if (sc.Match('-','-')) {
            sc.SetState(SCE_HA_COMMENTLINE);
            sc.Forward(2);
            inDashes = true;
         }
         // Comment block
         else if (sc.Match('{','-')) {
            sc.SetState(StyleFromNestLevel(nestLevel));
            sc.Forward(2);
            nestLevel = 1;
         }
         // String
         else if (sc.ch == '\"') {
            sc.SetState(SCE_HA_STRING);
            sc.Forward();
         }
         // Character or quoted name
         else if (sc.ch == '\'') {
            sc.SetState(SCE_HA_CHARACTER);
            sc.Forward();

            if (options.allowQuotes) {
               // Quoted type ''T
               if (sc.ch=='\'' && IsAHaskellWordStart(sc.chNext)) {
                  sc.Forward();
                  sc.ChangeState(SCE_HA_IDENTIFIER);
               } else if (sc.chNext != '\'') {
                  // Quoted value or promoted constructor 'N
                  if (IsAHaskellWordStart(sc.ch)) {
                     sc.ChangeState(SCE_HA_IDENTIFIER);
                  // Promoted constructor operator ':~>
                  } else if (sc.ch == ':') {
                     sc.ChangeState(SCE_HA_OPERATOR);
                  // Promoted list or tuple '[T]
                  } else if (sc.ch == '[' || sc.ch== '(') {
                     styler.ColourTo(sc.currentPos - 1, SCE_HA_OPERATOR);
                     sc.ChangeState(SCE_HA_DEFAULT);
                  }
               }
            }
         }
         // Operator starting with '?' or an implicit parameter
         else if (sc.ch == '?') {
            sc.SetState(SCE_HA_OPERATOR);

            if (  options.implicitParams
               && IsAHaskellWordStart(sc.chNext)
               && !IsHaskellUpperCase(sc.chNext)) {
               sc.Forward();
               sc.ChangeState(SCE_HA_IDENTIFIER);
            }
         }
         // Preprocessor
         else if (sc.atLineStart && sc.ch == '#') {
            mode = HA_MODE_DEFAULT;
            sc.SetState(SCE_HA_PREPROCESSOR);
            sc.Forward();
         }
         // Operator
         else if (IsAnHaskellOperatorChar(sc.ch)) {
            mode = HA_MODE_DEFAULT;
            sc.SetState(SCE_HA_OPERATOR);
         }
         // Braces and punctuation
         else if (sc.ch == ',' || sc.ch == ';'
               || sc.ch == '(' || sc.ch == ')'
               || sc.ch == '[' || sc.ch == ']'
               || sc.ch == '{' || sc.ch == '}') {
            sc.SetState(SCE_HA_OPERATOR);
            sc.ForwardSetState(SCE_HA_DEFAULT);
         }
         // Keyword or Identifier
         else if (IsAHaskellWordStart(sc.ch)) {
            sc.SetState(SCE_HA_IDENTIFIER);
         // Something we don't care about
         } else {
            sc.Forward();
         }
      }
            // This branch should never be reached.
      else {
         assert(false);
         sc.Forward();
      }
   }
   styler.SetLineState(lineCurrent, (nestLevel << 3) | mode);
   sc.Complete();
}

static bool LineStartsWithACommentOrPreprocessor(int line, Accessor &styler) {
   int pos = styler.LineStart(line);
   int eol_pos = styler.LineStart(line + 1) - 1;

   for (int i = pos; i < eol_pos; i++) {
      int style = styler.StyleAt(i);

      if (IsCommentStyle(style) || style == SCE_HA_PREPROCESSOR) {
         return true;
      }

      int ch = styler[i];

      if (  ch != ' '
         && ch != '\t') {
         return false;
      }
   }
   return true;
}

void SCI_METHOD LexerHaskell::Fold(unsigned int startPos, int length, int // initStyle
                                  ,IDocument *pAccess) {
   if (!options.fold)
      return;

   Accessor styler(pAccess, NULL);

   const int maxPos = startPos + length;
   const int maxLines =
      maxPos == styler.Length()
         ? styler.GetLine(maxPos)
         : styler.GetLine(maxPos - 1);  // Requested last line
   const int docLines = styler.GetLine(styler.Length()); // Available last line

   // Backtrack to previous non-blank line so we can determine indent level
   // for any white space lines
   // and so we can fix any preceding fold level (which is why we go back
   // at least one line in all cases)
   int spaceFlags = 0;
   int lineCurrent = styler.GetLine(startPos);
   bool importHere = LineContainsImport(lineCurrent, styler);
   int indentCurrent = styler.IndentAmount(lineCurrent, &spaceFlags, NULL);

   while (lineCurrent > 0) {
      lineCurrent--;
      importHere = LineContainsImport(lineCurrent, styler);
      indentCurrent = styler.IndentAmount(lineCurrent, &spaceFlags, NULL);
      if (!(indentCurrent & SC_FOLDLEVELWHITEFLAG) &&
               !LineStartsWithACommentOrPreprocessor(lineCurrent, styler))
         break;
   }

   int indentCurrentLevel = indentCurrent & SC_FOLDLEVELNUMBERMASK;
   int indentCurrentMask = indentCurrent & ~SC_FOLDLEVELNUMBERMASK;

   if (indentCurrentLevel != (SC_FOLDLEVELBASE & SC_FOLDLEVELNUMBERMASK)) {
      indentCurrent = (indentCurrentLevel + INDENT_OFFSET) | indentCurrentMask;
   }

   if (lineCurrent <= firstImportLine) {
      firstImportLine = -1; // readjust first import position
   }

   if (importHere) {
      if (firstImportLine == -1) {
         firstImportLine = lineCurrent;
      }
      if (firstImportLine != lineCurrent) {
         indentCurrentLevel++;
      }
      indentCurrent = indentCurrentLevel | indentCurrentMask;
   }

   // Process all characters to end of requested range
   //that hangs over the end of the range.  Cap processing in all cases
   // to end of document.
   while (lineCurrent <= docLines && lineCurrent <= maxLines) {

      // Gather info
      int lineNext = lineCurrent + 1;
      importHere = LineContainsImport(lineNext, styler);
      int indentNext = indentCurrent;

      if (lineNext <= docLines) {
         // Information about next line is only available if not at end of document
         indentNext = styler.IndentAmount(lineNext, &spaceFlags, NULL);
      }
      if (indentNext & SC_FOLDLEVELWHITEFLAG)
         indentNext = SC_FOLDLEVELWHITEFLAG | indentCurrentLevel;

      // Skip past any blank lines for next indent level info; we skip also
      // comments (all comments, not just those starting in column 0)
      // which effectively folds them into surrounding code rather
      // than screwing up folding.

      while ((lineNext < docLines) &&
            ((indentNext & SC_FOLDLEVELWHITEFLAG) ||
             (lineNext <= docLines && LineStartsWithACommentOrPreprocessor(lineNext, styler)))) {
         lineNext++;
         importHere = LineContainsImport(lineNext, styler);
         indentNext = styler.IndentAmount(lineNext, &spaceFlags, NULL);
      }

      int indentNextLevel = indentNext & SC_FOLDLEVELNUMBERMASK;
      int indentNextMask = indentNext & ~SC_FOLDLEVELNUMBERMASK;

      if (indentNextLevel != (SC_FOLDLEVELBASE & SC_FOLDLEVELNUMBERMASK)) {
         indentNext = (indentNextLevel + INDENT_OFFSET) | indentNextMask;
      }

      if (importHere) {
         if (firstImportLine == -1) {
            firstImportLine = lineNext;
         }
         if (firstImportLine != lineNext) {
            indentNextLevel++;
         }
         indentNext = indentNextLevel | indentNextMask;
      }

      const int levelBeforeComments = Maximum(indentCurrentLevel,indentNextLevel);

      // Now set all the indent levels on the lines we skipped
      // Do this from end to start.  Once we encounter one line
      // which is indented more than the line after the end of
      // the comment-block, use the level of the block before

      int skipLine = lineNext;
      int skipLevel = indentNextLevel;

      while (--skipLine > lineCurrent) {
         int skipLineIndent = styler.IndentAmount(skipLine, &spaceFlags, NULL);

         if (options.foldCompact) {
            if ((skipLineIndent & SC_FOLDLEVELNUMBERMASK) > indentNextLevel) {
               skipLevel = levelBeforeComments;
            }

            int whiteFlag = skipLineIndent & SC_FOLDLEVELWHITEFLAG;

            styler.SetLevel(skipLine, skipLevel | whiteFlag);
         } else {
            if (  (skipLineIndent & SC_FOLDLEVELNUMBERMASK) > indentNextLevel
               && !(skipLineIndent & SC_FOLDLEVELWHITEFLAG)
               && !LineStartsWithACommentOrPreprocessor(skipLine, styler)) {
               skipLevel = levelBeforeComments;
            }

            styler.SetLevel(skipLine, skipLevel);
         }
      }

      int lev = indentCurrent;

      if (!(indentCurrent & SC_FOLDLEVELWHITEFLAG)) {
         if ((indentCurrent & SC_FOLDLEVELNUMBERMASK) < (indentNext & SC_FOLDLEVELNUMBERMASK))
            lev |= SC_FOLDLEVELHEADERFLAG;
      }

      // Set fold level for this line and move to next line
      styler.SetLevel(lineCurrent, options.foldCompact ? lev : lev & ~SC_FOLDLEVELWHITEFLAG);
      indentCurrent = indentNext;
      lineCurrent = lineNext;
   }

   // NOTE: Cannot set level of last line here because indentCurrent doesn't have
   // header flag set; the loop above is crafted to take care of this case!
   //styler.SetLevel(lineCurrent, indentCurrent);
}

LexerModule lmHaskell(SCLEX_HASKELL, LexerHaskell::LexerFactoryHaskell, "haskell", haskellWordListDesc);

#ifdef HASKELL_UNICODE

// Unicode tables copied from https://github.com/ghc/packages-base/blob/master/cbits/WCsubst.c

/*-------------------------------------------------------------------------
This is an automatically generated file: do not edit
Generated by ubconfc at Mon Feb  7 20:26:56 CET 2011
-------------------------------------------------------------------------*/

/* Unicode general categories, listed in the same order as in the Unicode
 * standard -- this must be the same order as in GHC.Unicode.
 */

enum {
    NUMCAT_LU,  /* Letter, Uppercase */
    NUMCAT_LL,  /* Letter, Lowercase */
    NUMCAT_LT,  /* Letter, Titlecase */
    NUMCAT_LM,  /* Letter, Modifier */
    NUMCAT_LO,  /* Letter, Other */
    NUMCAT_MN,  /* Mark, Non-Spacing */
    NUMCAT_MC,  /* Mark, Spacing Combining */
    NUMCAT_ME,  /* Mark, Enclosing */
    NUMCAT_ND,  /* Number, Decimal */
    NUMCAT_NL,  /* Number, Letter */
    NUMCAT_NO,  /* Number, Other */
    NUMCAT_PC,  /* Punctuation, Connector */
    NUMCAT_PD,  /* Punctuation, Dash */
    NUMCAT_PS,  /* Punctuation, Open */
    NUMCAT_PE,  /* Punctuation, Close */
    NUMCAT_PI,  /* Punctuation, Initial quote */
    NUMCAT_PF,  /* Punctuation, Final quote */
    NUMCAT_PO,  /* Punctuation, Other */
    NUMCAT_SM,  /* Symbol, Math */
    NUMCAT_SC,  /* Symbol, Currency */
    NUMCAT_SK,  /* Symbol, Modifier */
    NUMCAT_SO,  /* Symbol, Other */
    NUMCAT_ZS,  /* Separator, Space */
    NUMCAT_ZL,  /* Separator, Line */
    NUMCAT_ZP,  /* Separator, Paragraph */
    NUMCAT_CC,  /* Other, Control */
    NUMCAT_CF,  /* Other, Format */
    NUMCAT_CS,  /* Other, Surrogate */
    NUMCAT_CO,  /* Other, Private Use */
    NUMCAT_CN   /* Other, Not Assigned */
};

struct _convrule_
{
    unsigned int category;
    unsigned int catnumber;
    int possible;
    int updist;
    int lowdist;
    int titledist;
};

struct _charblock_
{
    int start;
    int length;
    const struct _convrule_ *rule;
};

#define GENCAT_LO 262144
#define GENCAT_PC 2048
#define GENCAT_PD 128
#define GENCAT_MN 2097152
#define GENCAT_PE 32
#define GENCAT_NL 16777216
#define GENCAT_PF 131072
#define GENCAT_LT 524288
#define GENCAT_NO 65536
#define GENCAT_LU 512
#define GENCAT_PI 16384
#define GENCAT_SC 8
#define GENCAT_PO 4
#define GENCAT_PS 16
#define GENCAT_SK 1024
#define GENCAT_SM 64
#define GENCAT_SO 8192
#define GENCAT_CC 1
#define GENCAT_CF 32768
#define GENCAT_CO 268435456
#define GENCAT_ZL 33554432
#define GENCAT_CS 134217728
#define GENCAT_ZP 67108864
#define GENCAT_ZS 2
#define GENCAT_MC 8388608
#define GENCAT_ME 4194304
#define GENCAT_ND 256
#define GENCAT_LL 4096
#define GENCAT_LM 1048576
#define MAX_UNI_CHAR 1114109
#define NUM_BLOCKS 2783
#define NUM_CONVBLOCKS 1230
#define NUM_SPACEBLOCKS 8
#define NUM_LAT1BLOCKS 63
#define NUM_RULES 167
static const struct _convrule_ rule160={GENCAT_LL, NUMCAT_LL, 1, -7264, 0, -7264};
static const struct _convrule_ rule36={GENCAT_LU, NUMCAT_LU, 1, 0, 211, 0};
static const struct _convrule_ rule25={GENCAT_LU, NUMCAT_LU, 1, 0, -121, 0};
static const struct _convrule_ rule18={GENCAT_LL, NUMCAT_LL, 1, 743, 0, 743};
static const struct _convrule_ rule108={GENCAT_LU, NUMCAT_LU, 1, 0, 80, 0};
static const struct _convrule_ rule50={GENCAT_LL, NUMCAT_LL, 1, -79, 0, -79};
static const struct _convrule_ rule106={GENCAT_LL, NUMCAT_LL, 1, -96, 0, -96};
static const struct _convrule_ rule79={GENCAT_LL, NUMCAT_LL, 1, -69, 0, -69};
static const struct _convrule_ rule126={GENCAT_LL, NUMCAT_LL, 1, 128, 0, 128};
static const struct _convrule_ rule119={GENCAT_LL, NUMCAT_LL, 1, -59, 0, -59};
static const struct _convrule_ rule102={GENCAT_LL, NUMCAT_LL, 1, -86, 0, -86};
static const struct _convrule_ rule38={GENCAT_LL, NUMCAT_LL, 1, 163, 0, 163};
static const struct _convrule_ rule113={GENCAT_LL, NUMCAT_LL, 1, -48, 0, -48};
static const struct _convrule_ rule133={GENCAT_LL, NUMCAT_LL, 1, -7205, 0, -7205};
static const struct _convrule_ rule128={GENCAT_LL, NUMCAT_LL, 1, 126, 0, 126};
static const struct _convrule_ rule97={GENCAT_LL, NUMCAT_LL, 1, -57, 0, -57};
static const struct _convrule_ rule161={GENCAT_LU, NUMCAT_LU, 1, 0, -35332, 0};
static const struct _convrule_ rule136={GENCAT_LU, NUMCAT_LU, 1, 0, -112, 0};
static const struct _convrule_ rule99={GENCAT_LL, NUMCAT_LL, 1, -47, 0, -47};
static const struct _convrule_ rule90={GENCAT_LL, NUMCAT_LL, 1, -38, 0, -38};
static const struct _convrule_ rule32={GENCAT_LU, NUMCAT_LU, 1, 0, 202, 0};
static const struct _convrule_ rule145={GENCAT_LL, NUMCAT_LL, 1, -28, 0, -28};
static const struct _convrule_ rule93={GENCAT_LL, NUMCAT_LL, 1, -64, 0, -64};
static const struct _convrule_ rule91={GENCAT_LL, NUMCAT_LL, 1, -37, 0, -37};
static const struct _convrule_ rule60={GENCAT_LU, NUMCAT_LU, 1, 0, 71, 0};
static const struct _convrule_ rule100={GENCAT_LL, NUMCAT_LL, 1, -54, 0, -54};
static const struct _convrule_ rule94={GENCAT_LL, NUMCAT_LL, 1, -63, 0, -63};
static const struct _convrule_ rule35={GENCAT_LL, NUMCAT_LL, 1, 97, 0, 97};
static const struct _convrule_ rule149={GENCAT_SO, NUMCAT_SO, 1, -26, 0, -26};
static const struct _convrule_ rule103={GENCAT_LL, NUMCAT_LL, 1, -80, 0, -80};
static const struct _convrule_ rule96={GENCAT_LL, NUMCAT_LL, 1, -62, 0, -62};
static const struct _convrule_ rule81={GENCAT_LL, NUMCAT_LL, 1, -71, 0, -71};
static const struct _convrule_ rule9={GENCAT_LU, NUMCAT_LU, 1, 0, 32, 0};
static const struct _convrule_ rule147={GENCAT_NL, NUMCAT_NL, 1, -16, 0, -16};
static const struct _convrule_ rule143={GENCAT_LU, NUMCAT_LU, 1, 0, -8262, 0};
static const struct _convrule_ rule127={GENCAT_LL, NUMCAT_LL, 1, 112, 0, 112};
static const struct _convrule_ rule124={GENCAT_LL, NUMCAT_LL, 1, 86, 0, 86};
static const struct _convrule_ rule40={GENCAT_LL, NUMCAT_LL, 1, 130, 0, 130};
static const struct _convrule_ rule20={GENCAT_LL, NUMCAT_LL, 1, 121, 0, 121};
static const struct _convrule_ rule158={GENCAT_LU, NUMCAT_LU, 1, 0, -10782, 0};
static const struct _convrule_ rule111={GENCAT_LL, NUMCAT_LL, 1, -15, 0, -15};
static const struct _convrule_ rule12={GENCAT_LL, NUMCAT_LL, 1, -32, 0, -32};
static const struct _convrule_ rule85={GENCAT_MN, NUMCAT_MN, 1, 84, 0, 84};
static const struct _convrule_ rule166={GENCAT_LL, NUMCAT_LL, 1, -40, 0, -40};
static const struct _convrule_ rule125={GENCAT_LL, NUMCAT_LL, 1, 100, 0, 100};
static const struct _convrule_ rule123={GENCAT_LL, NUMCAT_LL, 1, 74, 0, 74};
static const struct _convrule_ rule92={GENCAT_LL, NUMCAT_LL, 1, -31, 0, -31};
static const struct _convrule_ rule56={GENCAT_LU, NUMCAT_LU, 1, 0, 10792, 0};
static const struct _convrule_ rule46={GENCAT_LL, NUMCAT_LL, 1, 56, 0, 56};
static const struct _convrule_ rule33={GENCAT_LU, NUMCAT_LU, 1, 0, 203, 0};
static const struct _convrule_ rule150={GENCAT_LU, NUMCAT_LU, 1, 0, -10743, 0};
static const struct _convrule_ rule39={GENCAT_LU, NUMCAT_LU, 1, 0, 213, 0};
static const struct _convrule_ rule57={GENCAT_LL, NUMCAT_LL, 1, 10815, 0, 10815};
static const struct _convrule_ rule157={GENCAT_LU, NUMCAT_LU, 1, 0, -10783, 0};
static const struct _convrule_ rule55={GENCAT_LU, NUMCAT_LU, 1, 0, -163, 0};
static const struct _convrule_ rule151={GENCAT_LU, NUMCAT_LU, 1, 0, -3814, 0};
static const struct _convrule_ rule142={GENCAT_LU, NUMCAT_LU, 1, 0, -8383, 0};
static const struct _convrule_ rule101={GENCAT_LL, NUMCAT_LL, 1, -8, 0, -8};
static const struct _convrule_ rule89={GENCAT_LU, NUMCAT_LU, 1, 0, 63, 0};
static const struct _convrule_ rule41={GENCAT_LU, NUMCAT_LU, 1, 0, 214, 0};
static const struct _convrule_ rule118={GENCAT_LL, NUMCAT_LL, 1, 3814, 0, 3814};
static const struct _convrule_ rule26={GENCAT_LL, NUMCAT_LL, 1, -300, 0, -300};
static const struct _convrule_ rule159={GENCAT_LU, NUMCAT_LU, 1, 0, -10815, 0};
static const struct _convrule_ rule115={GENCAT_LU, NUMCAT_LU, 1, 0, 7264, 0};
static const struct _convrule_ rule22={GENCAT_LL, NUMCAT_LL, 1, -1, 0, -1};
static const struct _convrule_ rule120={GENCAT_LU, NUMCAT_LU, 1, 0, -7615, 0};
static const struct _convrule_ rule49={GENCAT_LL, NUMCAT_LL, 1, -2, 0, -1};
static const struct _convrule_ rule131={GENCAT_LU, NUMCAT_LU, 1, 0, -74, 0};
static const struct _convrule_ rule88={GENCAT_LU, NUMCAT_LU, 1, 0, 64, 0};
static const struct _convrule_ rule30={GENCAT_LU, NUMCAT_LU, 1, 0, 205, 0};
static const struct _convrule_ rule117={GENCAT_LL, NUMCAT_LL, 1, 35332, 0, 35332};
static const struct _convrule_ rule110={GENCAT_LU, NUMCAT_LU, 1, 0, 15, 0};
static const struct _convrule_ rule130={GENCAT_LL, NUMCAT_LL, 1, 9, 0, 9};
static const struct _convrule_ rule121={GENCAT_LL, NUMCAT_LL, 1, 8, 0, 8};
static const struct _convrule_ rule95={GENCAT_LU, NUMCAT_LU, 1, 0, 8, 0};
static const struct _convrule_ rule54={GENCAT_LU, NUMCAT_LU, 1, 0, 10795, 0};
static const struct _convrule_ rule29={GENCAT_LU, NUMCAT_LU, 1, 0, 206, 0};
static const struct _convrule_ rule138={GENCAT_LU, NUMCAT_LU, 1, 0, -126, 0};
static const struct _convrule_ rule104={GENCAT_LL, NUMCAT_LL, 1, 7, 0, 7};
static const struct _convrule_ rule58={GENCAT_LU, NUMCAT_LU, 1, 0, -195, 0};
static const struct _convrule_ rule146={GENCAT_NL, NUMCAT_NL, 1, 0, 16, 0};
static const struct _convrule_ rule148={GENCAT_SO, NUMCAT_SO, 1, 0, 26, 0};
static const struct _convrule_ rule70={GENCAT_LL, NUMCAT_LL, 1, 42280, 0, 42280};
static const struct _convrule_ rule107={GENCAT_LU, NUMCAT_LU, 1, 0, -7, 0};
static const struct _convrule_ rule52={GENCAT_LU, NUMCAT_LU, 1, 0, -56, 0};
static const struct _convrule_ rule153={GENCAT_LL, NUMCAT_LL, 1, -10795, 0, -10795};
static const struct _convrule_ rule152={GENCAT_LU, NUMCAT_LU, 1, 0, -10727, 0};
static const struct _convrule_ rule141={GENCAT_LU, NUMCAT_LU, 1, 0, -7517, 0};
static const struct _convrule_ rule34={GENCAT_LU, NUMCAT_LU, 1, 0, 207, 0};
static const struct _convrule_ rule164={GENCAT_CO, NUMCAT_CO, 0, 0, 0, 0};
static const struct _convrule_ rule84={GENCAT_MN, NUMCAT_MN, 0, 0, 0, 0};
static const struct _convrule_ rule16={GENCAT_CF, NUMCAT_CF, 0, 0, 0, 0};
static const struct _convrule_ rule45={GENCAT_LO, NUMCAT_LO, 0, 0, 0, 0};
static const struct _convrule_ rule13={GENCAT_SO, NUMCAT_SO, 0, 0, 0, 0};
static const struct _convrule_ rule17={GENCAT_NO, NUMCAT_NO, 0, 0, 0, 0};
static const struct _convrule_ rule8={GENCAT_ND, NUMCAT_ND, 0, 0, 0, 0};
static const struct _convrule_ rule14={GENCAT_LL, NUMCAT_LL, 0, 0, 0, 0};
static const struct _convrule_ rule98={GENCAT_LU, NUMCAT_LU, 0, 0, 0, 0};
static const struct _convrule_ rule6={GENCAT_SM, NUMCAT_SM, 0, 0, 0, 0};
static const struct _convrule_ rule114={GENCAT_MC, NUMCAT_MC, 0, 0, 0, 0};
static const struct _convrule_ rule2={GENCAT_PO, NUMCAT_PO, 0, 0, 0, 0};
static const struct _convrule_ rule116={GENCAT_NL, NUMCAT_NL, 0, 0, 0, 0};
static const struct _convrule_ rule3={GENCAT_SC, NUMCAT_SC, 0, 0, 0, 0};
static const struct _convrule_ rule10={GENCAT_SK, NUMCAT_SK, 0, 0, 0, 0};
static const struct _convrule_ rule83={GENCAT_LM, NUMCAT_LM, 0, 0, 0, 0};
static const struct _convrule_ rule5={GENCAT_PE, NUMCAT_PE, 0, 0, 0, 0};
static const struct _convrule_ rule4={GENCAT_PS, NUMCAT_PS, 0, 0, 0, 0};
static const struct _convrule_ rule11={GENCAT_PC, NUMCAT_PC, 0, 0, 0, 0};
static const struct _convrule_ rule7={GENCAT_PD, NUMCAT_PD, 0, 0, 0, 0};
static const struct _convrule_ rule163={GENCAT_CS, NUMCAT_CS, 0, 0, 0, 0};
static const struct _convrule_ rule109={GENCAT_ME, NUMCAT_ME, 0, 0, 0, 0};
static const struct _convrule_ rule1={GENCAT_ZS, NUMCAT_ZS, 0, 0, 0, 0};
static const struct _convrule_ rule19={GENCAT_PF, NUMCAT_PF, 0, 0, 0, 0};
static const struct _convrule_ rule15={GENCAT_PI, NUMCAT_PI, 0, 0, 0, 0};
static const struct _convrule_ rule140={GENCAT_ZP, NUMCAT_ZP, 0, 0, 0, 0};
static const struct _convrule_ rule139={GENCAT_ZL, NUMCAT_ZL, 0, 0, 0, 0};
static const struct _convrule_ rule134={GENCAT_LU, NUMCAT_LU, 1, 0, -86, 0};
static const struct _convrule_ rule43={GENCAT_LU, NUMCAT_LU, 1, 0, 217, 0};
static const struct _convrule_ rule0={GENCAT_CC, NUMCAT_CC, 0, 0, 0, 0};
static const struct _convrule_ rule154={GENCAT_LL, NUMCAT_LL, 1, -10792, 0, -10792};
static const struct _convrule_ rule74={GENCAT_LL, NUMCAT_LL, 1, 10749, 0, 10749};
static const struct _convrule_ rule87={GENCAT_LU, NUMCAT_LU, 1, 0, 37, 0};
static const struct _convrule_ rule61={GENCAT_LL, NUMCAT_LL, 1, 10783, 0, 10783};
static const struct _convrule_ rule122={GENCAT_LU, NUMCAT_LU, 1, 0, -8, 0};
static const struct _convrule_ rule129={GENCAT_LT, NUMCAT_LT, 1, 0, -8, 0};
static const struct _convrule_ rule63={GENCAT_LL, NUMCAT_LL, 1, 10782, 0, 10782};
static const struct _convrule_ rule82={GENCAT_LL, NUMCAT_LL, 1, -219, 0, -219};
static const struct _convrule_ rule77={GENCAT_LL, NUMCAT_LL, 1, 10727, 0, 10727};
static const struct _convrule_ rule78={GENCAT_LL, NUMCAT_LL, 1, -218, 0, -218};
static const struct _convrule_ rule71={GENCAT_LL, NUMCAT_LL, 1, -209, 0, -209};
static const struct _convrule_ rule62={GENCAT_LL, NUMCAT_LL, 1, 10780, 0, 10780};
static const struct _convrule_ rule48={GENCAT_LT, NUMCAT_LT, 1, -1, 1, 0};
static const struct _convrule_ rule21={GENCAT_LU, NUMCAT_LU, 1, 0, 1, 0};
static const struct _convrule_ rule137={GENCAT_LU, NUMCAT_LU, 1, 0, -128, 0};
static const struct _convrule_ rule80={GENCAT_LL, NUMCAT_LL, 1, -217, 0, -217};
static const struct _convrule_ rule73={GENCAT_LL, NUMCAT_LL, 1, 10743, 0, 10743};
static const struct _convrule_ rule42={GENCAT_LU, NUMCAT_LU, 1, 0, 218, 0};
static const struct _convrule_ rule69={GENCAT_LL, NUMCAT_LL, 1, -207, 0, -207};
static const struct _convrule_ rule51={GENCAT_LU, NUMCAT_LU, 1, 0, -97, 0};
static const struct _convrule_ rule144={GENCAT_LU, NUMCAT_LU, 1, 0, 28, 0};
static const struct _convrule_ rule65={GENCAT_LL, NUMCAT_LL, 1, -206, 0, -206};
static const struct _convrule_ rule86={GENCAT_LU, NUMCAT_LU, 1, 0, 38, 0};
static const struct _convrule_ rule76={GENCAT_LL, NUMCAT_LL, 1, -214, 0, -214};
static const struct _convrule_ rule66={GENCAT_LL, NUMCAT_LL, 1, -205, 0, -205};
static const struct _convrule_ rule24={GENCAT_LL, NUMCAT_LL, 1, -232, 0, -232};
static const struct _convrule_ rule112={GENCAT_LU, NUMCAT_LU, 1, 0, 48, 0};
static const struct _convrule_ rule132={GENCAT_LT, NUMCAT_LT, 1, 0, -9, 0};
static const struct _convrule_ rule75={GENCAT_LL, NUMCAT_LL, 1, -213, 0, -213};
static const struct _convrule_ rule68={GENCAT_LL, NUMCAT_LL, 1, -203, 0, -203};
static const struct _convrule_ rule135={GENCAT_LU, NUMCAT_LU, 1, 0, -100, 0};
static const struct _convrule_ rule72={GENCAT_LL, NUMCAT_LL, 1, -211, 0, -211};
static const struct _convrule_ rule67={GENCAT_LL, NUMCAT_LL, 1, -202, 0, -202};
static const struct _convrule_ rule47={GENCAT_LU, NUMCAT_LU, 1, 0, 2, 1};
static const struct _convrule_ rule37={GENCAT_LU, NUMCAT_LU, 1, 0, 209, 0};
static const struct _convrule_ rule156={GENCAT_LU, NUMCAT_LU, 1, 0, -10749, 0};
static const struct _convrule_ rule64={GENCAT_LL, NUMCAT_LL, 1, -210, 0, -210};
static const struct _convrule_ rule44={GENCAT_LU, NUMCAT_LU, 1, 0, 219, 0};
static const struct _convrule_ rule28={GENCAT_LU, NUMCAT_LU, 1, 0, 210, 0};
static const struct _convrule_ rule53={GENCAT_LU, NUMCAT_LU, 1, 0, -130, 0};
static const struct _convrule_ rule165={GENCAT_LU, NUMCAT_LU, 1, 0, 40, 0};
static const struct _convrule_ rule162={GENCAT_LU, NUMCAT_LU, 1, 0, -42280, 0};
static const struct _convrule_ rule155={GENCAT_LU, NUMCAT_LU, 1, 0, -10780, 0};
static const struct _convrule_ rule105={GENCAT_LU, NUMCAT_LU, 1, 0, -60, 0};
static const struct _convrule_ rule59={GENCAT_LU, NUMCAT_LU, 1, 0, 69, 0};
static const struct _convrule_ rule31={GENCAT_LU, NUMCAT_LU, 1, 0, 79, 0};
static const struct _convrule_ rule27={GENCAT_LL, NUMCAT_LL, 1, 195, 0, 195};
static const struct _convrule_ rule23={GENCAT_LU, NUMCAT_LU, 1, 0, -199, 0};
static const struct _charblock_ allchars[]={
    {0, 32, &rule0},
    {32, 1, &rule1},
    {33, 3, &rule2},
    {36, 1, &rule3},
    {37, 3, &rule2},
    {40, 1, &rule4},
    {41, 1, &rule5},
    {42, 1, &rule2},
    {43, 1, &rule6},
    {44, 1, &rule2},
    {45, 1, &rule7},
    {46, 2, &rule2},
    {48, 10, &rule8},
    {58, 2, &rule2},
    {60, 3, &rule6},
    {63, 2, &rule2},
    {65, 26, &rule9},
    {91, 1, &rule4},
    {92, 1, &rule2},
    {93, 1, &rule5},
    {94, 1, &rule10},
    {95, 1, &rule11},
    {96, 1, &rule10},
    {97, 26, &rule12},
    {123, 1, &rule4},
    {124, 1, &rule6},
    {125, 1, &rule5},
    {126, 1, &rule6},
    {127, 33, &rule0},
    {160, 1, &rule1},
    {161, 1, &rule2},
    {162, 4, &rule3},
    {166, 2, &rule13},
    {168, 1, &rule10},
    {169, 1, &rule13},
    {170, 1, &rule14},
    {171, 1, &rule15},
    {172, 1, &rule6},
    {173, 1, &rule16},
    {174, 1, &rule13},
    {175, 1, &rule10},
    {176, 1, &rule13},
    {177, 1, &rule6},
    {178, 2, &rule17},
    {180, 1, &rule10},
    {181, 1, &rule18},
    {182, 1, &rule13},
    {183, 1, &rule2},
    {184, 1, &rule10},
    {185, 1, &rule17},
    {186, 1, &rule14},
    {187, 1, &rule19},
    {188, 3, &rule17},
    {191, 1, &rule2},
    {192, 23, &rule9},
    {215, 1, &rule6},
    {216, 7, &rule9},
    {223, 1, &rule14},
    {224, 23, &rule12},
    {247, 1, &rule6},
    {248, 7, &rule12},
    {255, 1, &rule20},
    {256, 1, &rule21},
    {257, 1, &rule22},
    {258, 1, &rule21},
    {259, 1, &rule22},
    {260, 1, &rule21},
    {261, 1, &rule22},
    {262, 1, &rule21},
    {263, 1, &rule22},
    {264, 1, &rule21},
    {265, 1, &rule22},
    {266, 1, &rule21},
    {267, 1, &rule22},
    {268, 1, &rule21},
    {269, 1, &rule22},
    {270, 1, &rule21},
    {271, 1, &rule22},
    {272, 1, &rule21},
    {273, 1, &rule22},
    {274, 1, &rule21},
    {275, 1, &rule22},
    {276, 1, &rule21},
    {277, 1, &rule22},
    {278, 1, &rule21},
    {279, 1, &rule22},
    {280, 1, &rule21},
    {281, 1, &rule22},
    {282, 1, &rule21},
    {283, 1, &rule22},
    {284, 1, &rule21},
    {285, 1, &rule22},
    {286, 1, &rule21},
    {287, 1, &rule22},
    {288, 1, &rule21},
    {289, 1, &rule22},
    {290, 1, &rule21},
    {291, 1, &rule22},
    {292, 1, &rule21},
    {293, 1, &rule22},
    {294, 1, &rule21},
    {295, 1, &rule22},
    {296, 1, &rule21},
    {297, 1, &rule22},
    {298, 1, &rule21},
    {299, 1, &rule22},
    {300, 1, &rule21},
    {301, 1, &rule22},
    {302, 1, &rule21},
    {303, 1, &rule22},
    {304, 1, &rule23},
    {305, 1, &rule24},
    {306, 1, &rule21},
    {307, 1, &rule22},
    {308, 1, &rule21},
    {309, 1, &rule22},
    {310, 1, &rule21},
    {311, 1, &rule22},
    {312, 1, &rule14},
    {313, 1, &rule21},
    {314, 1, &rule22},
    {315, 1, &rule21},
    {316, 1, &rule22},
    {317, 1, &rule21},
    {318, 1, &rule22},
    {319, 1, &rule21},
    {320, 1, &rule22},
    {321, 1, &rule21},
    {322, 1, &rule22},
    {323, 1, &rule21},
    {324, 1, &rule22},
    {325, 1, &rule21},
    {326, 1, &rule22},
    {327, 1, &rule21},
    {328, 1, &rule22},
    {329, 1, &rule14},
    {330, 1, &rule21},
    {331, 1, &rule22},
    {332, 1, &rule21},
    {333, 1, &rule22},
    {334, 1, &rule21},
    {335, 1, &rule22},
    {336, 1, &rule21},
    {337, 1, &rule22},
    {338, 1, &rule21},
    {339, 1, &rule22},
    {340, 1, &rule21},
    {341, 1, &rule22},
    {342, 1, &rule21},
    {343, 1, &rule22},
    {344, 1, &rule21},
    {345, 1, &rule22},
    {346, 1, &rule21},
    {347, 1, &rule22},
    {348, 1, &rule21},
    {349, 1, &rule22},
    {350, 1, &rule21},
    {351, 1, &rule22},
    {352, 1, &rule21},
    {353, 1, &rule22},
    {354, 1, &rule21},
    {355, 1, &rule22},
    {356, 1, &rule21},
    {357, 1, &rule22},
    {358, 1, &rule21},
    {359, 1, &rule22},
    {360, 1, &rule21},
    {361, 1, &rule22},
    {362, 1, &rule21},
    {363, 1, &rule22},
    {364, 1, &rule21},
    {365, 1, &rule22},
    {366, 1, &rule21},
    {367, 1, &rule22},
    {368, 1, &rule21},
    {369, 1, &rule22},
    {370, 1, &rule21},
    {371, 1, &rule22},
    {372, 1, &rule21},
    {373, 1, &rule22},
    {374, 1, &rule21},
    {375, 1, &rule22},
    {376, 1, &rule25},
    {377, 1, &rule21},
    {378, 1, &rule22},
    {379, 1, &rule21},
    {380, 1, &rule22},
    {381, 1, &rule21},
    {382, 1, &rule22},
    {383, 1, &rule26},
    {384, 1, &rule27},
    {385, 1, &rule28},
    {386, 1, &rule21},
    {387, 1, &rule22},
    {388, 1, &rule21},
    {389, 1, &rule22},
    {390, 1, &rule29},
    {391, 1, &rule21},
    {392, 1, &rule22},
    {393, 2, &rule30},
    {395, 1, &rule21},
    {396, 1, &rule22},
    {397, 1, &rule14},
    {398, 1, &rule31},
    {399, 1, &rule32},
    {400, 1, &rule33},
    {401, 1, &rule21},
    {402, 1, &rule22},
    {403, 1, &rule30},
    {404, 1, &rule34},
    {405, 1, &rule35},
    {406, 1, &rule36},
    {407, 1, &rule37},
    {408, 1, &rule21},
    {409, 1, &rule22},
    {410, 1, &rule38},
    {411, 1, &rule14},
    {412, 1, &rule36},
    {413, 1, &rule39},
    {414, 1, &rule40},
    {415, 1, &rule41},
    {416, 1, &rule21},
    {417, 1, &rule22},
    {418, 1, &rule21},
    {419, 1, &rule22},
    {420, 1, &rule21},
    {421, 1, &rule22},
    {422, 1, &rule42},
    {423, 1, &rule21},
    {424, 1, &rule22},
    {425, 1, &rule42},
    {426, 2, &rule14},
    {428, 1, &rule21},
    {429, 1, &rule22},
    {430, 1, &rule42},
    {431, 1, &rule21},
    {432, 1, &rule22},
    {433, 2, &rule43},
    {435, 1, &rule21},
    {436, 1, &rule22},
    {437, 1, &rule21},
    {438, 1, &rule22},
    {439, 1, &rule44},
    {440, 1, &rule21},
    {441, 1, &rule22},
    {442, 1, &rule14},
    {443, 1, &rule45},
    {444, 1, &rule21},
    {445, 1, &rule22},
    {446, 1, &rule14},
    {447, 1, &rule46},
    {448, 4, &rule45},
    {452, 1, &rule47},
    {453, 1, &rule48},
    {454, 1, &rule49},
    {455, 1, &rule47},
    {456, 1, &rule48},
    {457, 1, &rule49},
    {458, 1, &rule47},
    {459, 1, &rule48},
    {460, 1, &rule49},
    {461, 1, &rule21},
    {462, 1, &rule22},
    {463, 1, &rule21},
    {464, 1, &rule22},
    {465, 1, &rule21},
    {466, 1, &rule22},
    {467, 1, &rule21},
    {468, 1, &rule22},
    {469, 1, &rule21},
    {470, 1, &rule22},
    {471, 1, &rule21},
    {472, 1, &rule22},
    {473, 1, &rule21},
    {474, 1, &rule22},
    {475, 1, &rule21},
    {476, 1, &rule22},
    {477, 1, &rule50},
    {478, 1, &rule21},
    {479, 1, &rule22},
    {480, 1, &rule21},
    {481, 1, &rule22},
    {482, 1, &rule21},
    {483, 1, &rule22},
    {484, 1, &rule21},
    {485, 1, &rule22},
    {486, 1, &rule21},
    {487, 1, &rule22},
    {488, 1, &rule21},
    {489, 1, &rule22},
    {490, 1, &rule21},
    {491, 1, &rule22},
    {492, 1, &rule21},
    {493, 1, &rule22},
    {494, 1, &rule21},
    {495, 1, &rule22},
    {496, 1, &rule14},
    {497, 1, &rule47},
    {498, 1, &rule48},
    {499, 1, &rule49},
    {500, 1, &rule21},
    {501, 1, &rule22},
    {502, 1, &rule51},
    {503, 1, &rule52},
    {504, 1, &rule21},
    {505, 1, &rule22},
    {506, 1, &rule21},
    {507, 1, &rule22},
    {508, 1, &rule21},
    {509, 1, &rule22},
    {510, 1, &rule21},
    {511, 1, &rule22},
    {512, 1, &rule21},
    {513, 1, &rule22},
    {514, 1, &rule21},
    {515, 1, &rule22},
    {516, 1, &rule21},
    {517, 1, &rule22},
    {518, 1, &rule21},
    {519, 1, &rule22},
    {520, 1, &rule21},
    {521, 1, &rule22},
    {522, 1, &rule21},
    {523, 1, &rule22},
    {524, 1, &rule21},
    {525, 1, &rule22},
    {526, 1, &rule21},
    {527, 1, &rule22},
    {528, 1, &rule21},
    {529, 1, &rule22},
    {530, 1, &rule21},
    {531, 1, &rule22},
    {532, 1, &rule21},
    {533, 1, &rule22},
    {534, 1, &rule21},
    {535, 1, &rule22},
    {536, 1, &rule21},
    {537, 1, &rule22},
    {538, 1, &rule21},
    {539, 1, &rule22},
    {540, 1, &rule21},
    {541, 1, &rule22},
    {542, 1, &rule21},
    {543, 1, &rule22},
    {544, 1, &rule53},
    {545, 1, &rule14},
    {546, 1, &rule21},
    {547, 1, &rule22},
    {548, 1, &rule21},
    {549, 1, &rule22},
    {550, 1, &rule21},
    {551, 1, &rule22},
    {552, 1, &rule21},
    {553, 1, &rule22},
    {554, 1, &rule21},
    {555, 1, &rule22},
    {556, 1, &rule21},
    {557, 1, &rule22},
    {558, 1, &rule21},
    {559, 1, &rule22},
    {560, 1, &rule21},
    {561, 1, &rule22},
    {562, 1, &rule21},
    {563, 1, &rule22},
    {564, 6, &rule14},
    {570, 1, &rule54},
    {571, 1, &rule21},
    {572, 1, &rule22},
    {573, 1, &rule55},
    {574, 1, &rule56},
    {575, 2, &rule57},
    {577, 1, &rule21},
    {578, 1, &rule22},
    {579, 1, &rule58},
    {580, 1, &rule59},
    {581, 1, &rule60},
    {582, 1, &rule21},
    {583, 1, &rule22},
    {584, 1, &rule21},
    {585, 1, &rule22},
    {586, 1, &rule21},
    {587, 1, &rule22},
    {588, 1, &rule21},
    {589, 1, &rule22},
    {590, 1, &rule21},
    {591, 1, &rule22},
    {592, 1, &rule61},
    {593, 1, &rule62},
    {594, 1, &rule63},
    {595, 1, &rule64},
    {596, 1, &rule65},
    {597, 1, &rule14},
    {598, 2, &rule66},
    {600, 1, &rule14},
    {601, 1, &rule67},
    {602, 1, &rule14},
    {603, 1, &rule68},
    {604, 4, &rule14},
    {608, 1, &rule66},
    {609, 2, &rule14},
    {611, 1, &rule69},
    {612, 1, &rule14},
    {613, 1, &rule70},
    {614, 2, &rule14},
    {616, 1, &rule71},
    {617, 1, &rule72},
    {618, 1, &rule14},
    {619, 1, &rule73},
    {620, 3, &rule14},
    {623, 1, &rule72},
    {624, 1, &rule14},
    {625, 1, &rule74},
    {626, 1, &rule75},
    {627, 2, &rule14},
    {629, 1, &rule76},
    {630, 7, &rule14},
    {637, 1, &rule77},
    {638, 2, &rule14},
    {640, 1, &rule78},
    {641, 2, &rule14},
    {643, 1, &rule78},
    {644, 4, &rule14},
    {648, 1, &rule78},
    {649, 1, &rule79},
    {650, 2, &rule80},
    {652, 1, &rule81},
    {653, 5, &rule14},
    {658, 1, &rule82},
    {659, 1, &rule14},
    {660, 1, &rule45},
    {661, 27, &rule14},
    {688, 18, &rule83},
    {706, 4, &rule10},
    {710, 12, &rule83},
    {722, 14, &rule10},
    {736, 5, &rule83},
    {741, 7, &rule10},
    {748, 1, &rule83},
    {749, 1, &rule10},
    {750, 1, &rule83},
    {751, 17, &rule10},
    {768, 69, &rule84},
    {837, 1, &rule85},
    {838, 42, &rule84},
    {880, 1, &rule21},
    {881, 1, &rule22},
    {882, 1, &rule21},
    {883, 1, &rule22},
    {884, 1, &rule83},
    {885, 1, &rule10},
    {886, 1, &rule21},
    {887, 1, &rule22},
    {890, 1, &rule83},
    {891, 3, &rule40},
    {894, 1, &rule2},
    {900, 2, &rule10},
    {902, 1, &rule86},
    {903, 1, &rule2},
    {904, 3, &rule87},
    {908, 1, &rule88},
    {910, 2, &rule89},
    {912, 1, &rule14},
    {913, 17, &rule9},
    {931, 9, &rule9},
    {940, 1, &rule90},
    {941, 3, &rule91},
    {944, 1, &rule14},
    {945, 17, &rule12},
    {962, 1, &rule92},
    {963, 9, &rule12},
    {972, 1, &rule93},
    {973, 2, &rule94},
    {975, 1, &rule95},
    {976, 1, &rule96},
    {977, 1, &rule97},
    {978, 3, &rule98},
    {981, 1, &rule99},
    {982, 1, &rule100},
    {983, 1, &rule101},
    {984, 1, &rule21},
    {985, 1, &rule22},
    {986, 1, &rule21},
    {987, 1, &rule22},
    {988, 1, &rule21},
    {989, 1, &rule22},
    {990, 1, &rule21},
    {991, 1, &rule22},
    {992, 1, &rule21},
    {993, 1, &rule22},
    {994, 1, &rule21},
    {995, 1, &rule22},
    {996, 1, &rule21},
    {997, 1, &rule22},
    {998, 1, &rule21},
    {999, 1, &rule22},
    {1000, 1, &rule21},
    {1001, 1, &rule22},
    {1002, 1, &rule21},
    {1003, 1, &rule22},
    {1004, 1, &rule21},
    {1005, 1, &rule22},
    {1006, 1, &rule21},
    {1007, 1, &rule22},
    {1008, 1, &rule102},
    {1009, 1, &rule103},
    {1010, 1, &rule104},
    {1011, 1, &rule14},
    {1012, 1, &rule105},
    {1013, 1, &rule106},
    {1014, 1, &rule6},
    {1015, 1, &rule21},
    {1016, 1, &rule22},
    {1017, 1, &rule107},
    {1018, 1, &rule21},
    {1019, 1, &rule22},
    {1020, 1, &rule14},
    {1021, 3, &rule53},
    {1024, 16, &rule108},
    {1040, 32, &rule9},
    {1072, 32, &rule12},
    {1104, 16, &rule103},
    {1120, 1, &rule21},
    {1121, 1, &rule22},
    {1122, 1, &rule21},
    {1123, 1, &rule22},
    {1124, 1, &rule21},
    {1125, 1, &rule22},
    {1126, 1, &rule21},
    {1127, 1, &rule22},
    {1128, 1, &rule21},
    {1129, 1, &rule22},
    {1130, 1, &rule21},
    {1131, 1, &rule22},
    {1132, 1, &rule21},
    {1133, 1, &rule22},
    {1134, 1, &rule21},
    {1135, 1, &rule22},
    {1136, 1, &rule21},
    {1137, 1, &rule22},
    {1138, 1, &rule21},
    {1139, 1, &rule22},
    {1140, 1, &rule21},
    {1141, 1, &rule22},
    {1142, 1, &rule21},
    {1143, 1, &rule22},
    {1144, 1, &rule21},
    {1145, 1, &rule22},
    {1146, 1, &rule21},
    {1147, 1, &rule22},
    {1148, 1, &rule21},
    {1149, 1, &rule22},
    {1150, 1, &rule21},
    {1151, 1, &rule22},
    {1152, 1, &rule21},
    {1153, 1, &rule22},
    {1154, 1, &rule13},
    {1155, 5, &rule84},
    {1160, 2, &rule109},
    {1162, 1, &rule21},
    {1163, 1, &rule22},
    {1164, 1, &rule21},
    {1165, 1, &rule22},
    {1166, 1, &rule21},
    {1167, 1, &rule22},
    {1168, 1, &rule21},
    {1169, 1, &rule22},
    {1170, 1, &rule21},
    {1171, 1, &rule22},
    {1172, 1, &rule21},
    {1173, 1, &rule22},
    {1174, 1, &rule21},
    {1175, 1, &rule22},
    {1176, 1, &rule21},
    {1177, 1, &rule22},
    {1178, 1, &rule21},
    {1179, 1, &rule22},
    {1180, 1, &rule21},
    {1181, 1, &rule22},
    {1182, 1, &rule21},
    {1183, 1, &rule22},
    {1184, 1, &rule21},
    {1185, 1, &rule22},
    {1186, 1, &rule21},
    {1187, 1, &rule22},
    {1188, 1, &rule21},
    {1189, 1, &rule22},
    {1190, 1, &rule21},
    {1191, 1, &rule22},
    {1192, 1, &rule21},
    {1193, 1, &rule22},
    {1194, 1, &rule21},
    {1195, 1, &rule22},
    {1196, 1, &rule21},
    {1197, 1, &rule22},
    {1198, 1, &rule21},
    {1199, 1, &rule22},
    {1200, 1, &rule21},
    {1201, 1, &rule22},
    {1202, 1, &rule21},
    {1203, 1, &rule22},
    {1204, 1, &rule21},
    {1205, 1, &rule22},
    {1206, 1, &rule21},
    {1207, 1, &rule22},
    {1208, 1, &rule21},
    {1209, 1, &rule22},
    {1210, 1, &rule21},
    {1211, 1, &rule22},
    {1212, 1, &rule21},
    {1213, 1, &rule22},
    {1214, 1, &rule21},
    {1215, 1, &rule22},
    {1216, 1, &rule110},
    {1217, 1, &rule21},
    {1218, 1, &rule22},
    {1219, 1, &rule21},
    {1220, 1, &rule22},
    {1221, 1, &rule21},
    {1222, 1, &rule22},
    {1223, 1, &rule21},
    {1224, 1, &rule22},
    {1225, 1, &rule21},
    {1226, 1, &rule22},
    {1227, 1, &rule21},
    {1228, 1, &rule22},
    {1229, 1, &rule21},
    {1230, 1, &rule22},
    {1231, 1, &rule111},
    {1232, 1, &rule21},
    {1233, 1, &rule22},
    {1234, 1, &rule21},
    {1235, 1, &rule22},
    {1236, 1, &rule21},
    {1237, 1, &rule22},
    {1238, 1, &rule21},
    {1239, 1, &rule22},
    {1240, 1, &rule21},
    {1241, 1, &rule22},
    {1242, 1, &rule21},
    {1243, 1, &rule22},
    {1244, 1, &rule21},
    {1245, 1, &rule22},
    {1246, 1, &rule21},
    {1247, 1, &rule22},
    {1248, 1, &rule21},
    {1249, 1, &rule22},
    {1250, 1, &rule21},
    {1251, 1, &rule22},
    {1252, 1, &rule21},
    {1253, 1, &rule22},
    {1254, 1, &rule21},
    {1255, 1, &rule22},
    {1256, 1, &rule21},
    {1257, 1, &rule22},
    {1258, 1, &rule21},
    {1259, 1, &rule22},
    {1260, 1, &rule21},
    {1261, 1, &rule22},
    {1262, 1, &rule21},
    {1263, 1, &rule22},
    {1264, 1, &rule21},
    {1265, 1, &rule22},
    {1266, 1, &rule21},
    {1267, 1, &rule22},
    {1268, 1, &rule21},
    {1269, 1, &rule22},
    {1270, 1, &rule21},
    {1271, 1, &rule22},
    {1272, 1, &rule21},
    {1273, 1, &rule22},
    {1274, 1, &rule21},
    {1275, 1, &rule22},
    {1276, 1, &rule21},
    {1277, 1, &rule22},
    {1278, 1, &rule21},
    {1279, 1, &rule22},
    {1280, 1, &rule21},
    {1281, 1, &rule22},
    {1282, 1, &rule21},
    {1283, 1, &rule22},
    {1284, 1, &rule21},
    {1285, 1, &rule22},
    {1286, 1, &rule21},
    {1287, 1, &rule22},
    {1288, 1, &rule21},
    {1289, 1, &rule22},
    {1290, 1, &rule21},
    {1291, 1, &rule22},
    {1292, 1, &rule21},
    {1293, 1, &rule22},
    {1294, 1, &rule21},
    {1295, 1, &rule22},
    {1296, 1, &rule21},
    {1297, 1, &rule22},
    {1298, 1, &rule21},
    {1299, 1, &rule22},
    {1300, 1, &rule21},
    {1301, 1, &rule22},
    {1302, 1, &rule21},
    {1303, 1, &rule22},
    {1304, 1, &rule21},
    {1305, 1, &rule22},
    {1306, 1, &rule21},
    {1307, 1, &rule22},
    {1308, 1, &rule21},
    {1309, 1, &rule22},
    {1310, 1, &rule21},
    {1311, 1, &rule22},
    {1312, 1, &rule21},
    {1313, 1, &rule22},
    {1314, 1, &rule21},
    {1315, 1, &rule22},
    {1316, 1, &rule21},
    {1317, 1, &rule22},
    {1318, 1, &rule21},
    {1319, 1, &rule22},
    {1329, 38, &rule112},
    {1369, 1, &rule83},
    {1370, 6, &rule2},
    {1377, 38, &rule113},
    {1415, 1, &rule14},
    {1417, 1, &rule2},
    {1418, 1, &rule7},
    {1425, 45, &rule84},
    {1470, 1, &rule7},
    {1471, 1, &rule84},
    {1472, 1, &rule2},
    {1473, 2, &rule84},
    {1475, 1, &rule2},
    {1476, 2, &rule84},
    {1478, 1, &rule2},
    {1479, 1, &rule84},
    {1488, 27, &rule45},
    {1520, 3, &rule45},
    {1523, 2, &rule2},
    {1536, 4, &rule16},
    {1542, 3, &rule6},
    {1545, 2, &rule2},
    {1547, 1, &rule3},
    {1548, 2, &rule2},
    {1550, 2, &rule13},
    {1552, 11, &rule84},
    {1563, 1, &rule2},
    {1566, 2, &rule2},
    {1568, 32, &rule45},
    {1600, 1, &rule83},
    {1601, 10, &rule45},
    {1611, 21, &rule84},
    {1632, 10, &rule8},
    {1642, 4, &rule2},
    {1646, 2, &rule45},
    {1648, 1, &rule84},
    {1649, 99, &rule45},
    {1748, 1, &rule2},
    {1749, 1, &rule45},
    {1750, 7, &rule84},
    {1757, 1, &rule16},
    {1758, 1, &rule13},
    {1759, 6, &rule84},
    {1765, 2, &rule83},
    {1767, 2, &rule84},
    {1769, 1, &rule13},
    {1770, 4, &rule84},
    {1774, 2, &rule45},
    {1776, 10, &rule8},
    {1786, 3, &rule45},
    {1789, 2, &rule13},
    {1791, 1, &rule45},
    {1792, 14, &rule2},
    {1807, 1, &rule16},
    {1808, 1, &rule45},
    {1809, 1, &rule84},
    {1810, 30, &rule45},
    {1840, 27, &rule84},
    {1869, 89, &rule45},
    {1958, 11, &rule84},
    {1969, 1, &rule45},
    {1984, 10, &rule8},
    {1994, 33, &rule45},
    {2027, 9, &rule84},
    {2036, 2, &rule83},
    {2038, 1, &rule13},
    {2039, 3, &rule2},
    {2042, 1, &rule83},
    {2048, 22, &rule45},
    {2070, 4, &rule84},
    {2074, 1, &rule83},
    {2075, 9, &rule84},
    {2084, 1, &rule83},
    {2085, 3, &rule84},
    {2088, 1, &rule83},
    {2089, 5, &rule84},
    {2096, 15, &rule2},
    {2112, 25, &rule45},
    {2137, 3, &rule84},
    {2142, 1, &rule2},
    {2304, 3, &rule84},
    {2307, 1, &rule114},
    {2308, 54, &rule45},
    {2362, 1, &rule84},
    {2363, 1, &rule114},
    {2364, 1, &rule84},
    {2365, 1, &rule45},
    {2366, 3, &rule114},
    {2369, 8, &rule84},
    {2377, 4, &rule114},
    {2381, 1, &rule84},
    {2382, 2, &rule114},
    {2384, 1, &rule45},
    {2385, 7, &rule84},
    {2392, 10, &rule45},
    {2402, 2, &rule84},
    {2404, 2, &rule2},
    {2406, 10, &rule8},
    {2416, 1, &rule2},
    {2417, 1, &rule83},
    {2418, 6, &rule45},
    {2425, 7, &rule45},
    {2433, 1, &rule84},
    {2434, 2, &rule114},
    {2437, 8, &rule45},
    {2447, 2, &rule45},
    {2451, 22, &rule45},
    {2474, 7, &rule45},
    {2482, 1, &rule45},
    {2486, 4, &rule45},
    {2492, 1, &rule84},
    {2493, 1, &rule45},
    {2494, 3, &rule114},
    {2497, 4, &rule84},
    {2503, 2, &rule114},
    {2507, 2, &rule114},
    {2509, 1, &rule84},
    {2510, 1, &rule45},
    {2519, 1, &rule114},
    {2524, 2, &rule45},
    {2527, 3, &rule45},
    {2530, 2, &rule84},
    {2534, 10, &rule8},
    {2544, 2, &rule45},
    {2546, 2, &rule3},
    {2548, 6, &rule17},
    {2554, 1, &rule13},
    {2555, 1, &rule3},
    {2561, 2, &rule84},
    {2563, 1, &rule114},
    {2565, 6, &rule45},
    {2575, 2, &rule45},
    {2579, 22, &rule45},
    {2602, 7, &rule45},
    {2610, 2, &rule45},
    {2613, 2, &rule45},
    {2616, 2, &rule45},
    {2620, 1, &rule84},
    {2622, 3, &rule114},
    {2625, 2, &rule84},
    {2631, 2, &rule84},
    {2635, 3, &rule84},
    {2641, 1, &rule84},
    {2649, 4, &rule45},
    {2654, 1, &rule45},
    {2662, 10, &rule8},
    {2672, 2, &rule84},
    {2674, 3, &rule45},
    {2677, 1, &rule84},
    {2689, 2, &rule84},
    {2691, 1, &rule114},
    {2693, 9, &rule45},
    {2703, 3, &rule45},
    {2707, 22, &rule45},
    {2730, 7, &rule45},
    {2738, 2, &rule45},
    {2741, 5, &rule45},
    {2748, 1, &rule84},
    {2749, 1, &rule45},
    {2750, 3, &rule114},
    {2753, 5, &rule84},
    {2759, 2, &rule84},
    {2761, 1, &rule114},
    {2763, 2, &rule114},
    {2765, 1, &rule84},
    {2768, 1, &rule45},
    {2784, 2, &rule45},
    {2786, 2, &rule84},
    {2790, 10, &rule8},
    {2801, 1, &rule3},
    {2817, 1, &rule84},
    {2818, 2, &rule114},
    {2821, 8, &rule45},
    {2831, 2, &rule45},
    {2835, 22, &rule45},
    {2858, 7, &rule45},
    {2866, 2, &rule45},
    {2869, 5, &rule45},
    {2876, 1, &rule84},
    {2877, 1, &rule45},
    {2878, 1, &rule114},
    {2879, 1, &rule84},
    {2880, 1, &rule114},
    {2881, 4, &rule84},
    {2887, 2, &rule114},
    {2891, 2, &rule114},
    {2893, 1, &rule84},
    {2902, 1, &rule84},
    {2903, 1, &rule114},
    {2908, 2, &rule45},
    {2911, 3, &rule45},
    {2914, 2, &rule84},
    {2918, 10, &rule8},
    {2928, 1, &rule13},
    {2929, 1, &rule45},
    {2930, 6, &rule17},
    {2946, 1, &rule84},
    {2947, 1, &rule45},
    {2949, 6, &rule45},
    {2958, 3, &rule45},
    {2962, 4, &rule45},
    {2969, 2, &rule45},
    {2972, 1, &rule45},
    {2974, 2, &rule45},
    {2979, 2, &rule45},
    {2984, 3, &rule45},
    {2990, 12, &rule45},
    {3006, 2, &rule114},
    {3008, 1, &rule84},
    {3009, 2, &rule114},
    {3014, 3, &rule114},
    {3018, 3, &rule114},
    {3021, 1, &rule84},
    {3024, 1, &rule45},
    {3031, 1, &rule114},
    {3046, 10, &rule8},
    {3056, 3, &rule17},
    {3059, 6, &rule13},
    {3065, 1, &rule3},
    {3066, 1, &rule13},
    {3073, 3, &rule114},
    {3077, 8, &rule45},
    {3086, 3, &rule45},
    {3090, 23, &rule45},
    {3114, 10, &rule45},
    {3125, 5, &rule45},
    {3133, 1, &rule45},
    {3134, 3, &rule84},
    {3137, 4, &rule114},
    {3142, 3, &rule84},
    {3146, 4, &rule84},
    {3157, 2, &rule84},
    {3160, 2, &rule45},
    {3168, 2, &rule45},
    {3170, 2, &rule84},
    {3174, 10, &rule8},
    {3192, 7, &rule17},
    {3199, 1, &rule13},
    {3202, 2, &rule114},
    {3205, 8, &rule45},
    {3214, 3, &rule45},
    {3218, 23, &rule45},
    {3242, 10, &rule45},
    {3253, 5, &rule45},
    {3260, 1, &rule84},
    {3261, 1, &rule45},
    {3262, 1, &rule114},
    {3263, 1, &rule84},
    {3264, 5, &rule114},
    {3270, 1, &rule84},
    {3271, 2, &rule114},
    {3274, 2, &rule114},
    {3276, 2, &rule84},
    {3285, 2, &rule114},
    {3294, 1, &rule45},
    {3296, 2, &rule45},
    {3298, 2, &rule84},
    {3302, 10, &rule8},
    {3313, 2, &rule45},
    {3330, 2, &rule114},
    {3333, 8, &rule45},
    {3342, 3, &rule45},
    {3346, 41, &rule45},
    {3389, 1, &rule45},
    {3390, 3, &rule114},
    {3393, 4, &rule84},
    {3398, 3, &rule114},
    {3402, 3, &rule114},
    {3405, 1, &rule84},
    {3406, 1, &rule45},
    {3415, 1, &rule114},
    {3424, 2, &rule45},
    {3426, 2, &rule84},
    {3430, 10, &rule8},
    {3440, 6, &rule17},
    {3449, 1, &rule13},
    {3450, 6, &rule45},
    {3458, 2, &rule114},
    {3461, 18, &rule45},
    {3482, 24, &rule45},
    {3507, 9, &rule45},
    {3517, 1, &rule45},
    {3520, 7, &rule45},
    {3530, 1, &rule84},
    {3535, 3, &rule114},
    {3538, 3, &rule84},
    {3542, 1, &rule84},
    {3544, 8, &rule114},
    {3570, 2, &rule114},
    {3572, 1, &rule2},
    {3585, 48, &rule45},
    {3633, 1, &rule84},
    {3634, 2, &rule45},
    {3636, 7, &rule84},
    {3647, 1, &rule3},
    {3648, 6, &rule45},
    {3654, 1, &rule83},
    {3655, 8, &rule84},
    {3663, 1, &rule2},
    {3664, 10, &rule8},
    {3674, 2, &rule2},
    {3713, 2, &rule45},
    {3716, 1, &rule45},
    {3719, 2, &rule45},
    {3722, 1, &rule45},
    {3725, 1, &rule45},
    {3732, 4, &rule45},
    {3737, 7, &rule45},
    {3745, 3, &rule45},
    {3749, 1, &rule45},
    {3751, 1, &rule45},
    {3754, 2, &rule45},
    {3757, 4, &rule45},
    {3761, 1, &rule84},
    {3762, 2, &rule45},
    {3764, 6, &rule84},
    {3771, 2, &rule84},
    {3773, 1, &rule45},
    {3776, 5, &rule45},
    {3782, 1, &rule83},
    {3784, 6, &rule84},
    {3792, 10, &rule8},
    {3804, 2, &rule45},
    {3840, 1, &rule45},
    {3841, 3, &rule13},
    {3844, 15, &rule2},
    {3859, 5, &rule13},
    {3864, 2, &rule84},
    {3866, 6, &rule13},
    {3872, 10, &rule8},
    {3882, 10, &rule17},
    {3892, 1, &rule13},
    {3893, 1, &rule84},
    {3894, 1, &rule13},
    {3895, 1, &rule84},
    {3896, 1, &rule13},
    {3897, 1, &rule84},
    {3898, 1, &rule4},
    {3899, 1, &rule5},
    {3900, 1, &rule4},
    {3901, 1, &rule5},
    {3902, 2, &rule114},
    {3904, 8, &rule45},
    {3913, 36, &rule45},
    {3953, 14, &rule84},
    {3967, 1, &rule114},
    {3968, 5, &rule84},
    {3973, 1, &rule2},
    {3974, 2, &rule84},
    {3976, 5, &rule45},
    {3981, 11, &rule84},
    {3993, 36, &rule84},
    {4030, 8, &rule13},
    {4038, 1, &rule84},
    {4039, 6, &rule13},
    {4046, 2, &rule13},
    {4048, 5, &rule2},
    {4053, 4, &rule13},
    {4057, 2, &rule2},
    {4096, 43, &rule45},
    {4139, 2, &rule114},
    {4141, 4, &rule84},
    {4145, 1, &rule114},
    {4146, 6, &rule84},
    {4152, 1, &rule114},
    {4153, 2, &rule84},
    {4155, 2, &rule114},
    {4157, 2, &rule84},
    {4159, 1, &rule45},
    {4160, 10, &rule8},
    {4170, 6, &rule2},
    {4176, 6, &rule45},
    {4182, 2, &rule114},
    {4184, 2, &rule84},
    {4186, 4, &rule45},
    {4190, 3, &rule84},
    {4193, 1, &rule45},
    {4194, 3, &rule114},
    {4197, 2, &rule45},
    {4199, 7, &rule114},
    {4206, 3, &rule45},
    {4209, 4, &rule84},
    {4213, 13, &rule45},
    {4226, 1, &rule84},
    {4227, 2, &rule114},
    {4229, 2, &rule84},
    {4231, 6, &rule114},
    {4237, 1, &rule84},
    {4238, 1, &rule45},
    {4239, 1, &rule114},
    {4240, 10, &rule8},
    {4250, 3, &rule114},
    {4253, 1, &rule84},
    {4254, 2, &rule13},
    {4256, 38, &rule115},
    {4304, 43, &rule45},
    {4347, 1, &rule2},
    {4348, 1, &rule83},
    {4352, 329, &rule45},
    {4682, 4, &rule45},
    {4688, 7, &rule45},
    {4696, 1, &rule45},
    {4698, 4, &rule45},
    {4704, 41, &rule45},
    {4746, 4, &rule45},
    {4752, 33, &rule45},
    {4786, 4, &rule45},
    {4792, 7, &rule45},
    {4800, 1, &rule45},
    {4802, 4, &rule45},
    {4808, 15, &rule45},
    {4824, 57, &rule45},
    {4882, 4, &rule45},
    {4888, 67, &rule45},
    {4957, 3, &rule84},
    {4960, 1, &rule13},
    {4961, 8, &rule2},
    {4969, 20, &rule17},
    {4992, 16, &rule45},
    {5008, 10, &rule13},
    {5024, 85, &rule45},
    {5120, 1, &rule7},
    {5121, 620, &rule45},
    {5741, 2, &rule2},
    {5743, 17, &rule45},
    {5760, 1, &rule1},
    {5761, 26, &rule45},
    {5787, 1, &rule4},
    {5788, 1, &rule5},
    {5792, 75, &rule45},
    {5867, 3, &rule2},
    {5870, 3, &rule116},
    {5888, 13, &rule45},
    {5902, 4, &rule45},
    {5906, 3, &rule84},
    {5920, 18, &rule45},
    {5938, 3, &rule84},
    {5941, 2, &rule2},
    {5952, 18, &rule45},
    {5970, 2, &rule84},
    {5984, 13, &rule45},
    {5998, 3, &rule45},
    {6002, 2, &rule84},
    {6016, 52, &rule45},
    {6068, 2, &rule16},
    {6070, 1, &rule114},
    {6071, 7, &rule84},
    {6078, 8, &rule114},
    {6086, 1, &rule84},
    {6087, 2, &rule114},
    {6089, 11, &rule84},
    {6100, 3, &rule2},
    {6103, 1, &rule83},
    {6104, 3, &rule2},
    {6107, 1, &rule3},
    {6108, 1, &rule45},
    {6109, 1, &rule84},
    {6112, 10, &rule8},
    {6128, 10, &rule17},
    {6144, 6, &rule2},
    {6150, 1, &rule7},
    {6151, 4, &rule2},
    {6155, 3, &rule84},
    {6158, 1, &rule1},
    {6160, 10, &rule8},
    {6176, 35, &rule45},
    {6211, 1, &rule83},
    {6212, 52, &rule45},
    {6272, 41, &rule45},
    {6313, 1, &rule84},
    {6314, 1, &rule45},
    {6320, 70, &rule45},
    {6400, 29, &rule45},
    {6432, 3, &rule84},
    {6435, 4, &rule114},
    {6439, 2, &rule84},
    {6441, 3, &rule114},
    {6448, 2, &rule114},
    {6450, 1, &rule84},
    {6451, 6, &rule114},
    {6457, 3, &rule84},
    {6464, 1, &rule13},
    {6468, 2, &rule2},
    {6470, 10, &rule8},
    {6480, 30, &rule45},
    {6512, 5, &rule45},
    {6528, 44, &rule45},
    {6576, 17, &rule114},
    {6593, 7, &rule45},
    {6600, 2, &rule114},
    {6608, 10, &rule8},
    {6618, 1, &rule17},
    {6622, 34, &rule13},
    {6656, 23, &rule45},
    {6679, 2, &rule84},
    {6681, 3, &rule114},
    {6686, 2, &rule2},
    {6688, 53, &rule45},
    {6741, 1, &rule114},
    {6742, 1, &rule84},
    {6743, 1, &rule114},
    {6744, 7, &rule84},
    {6752, 1, &rule84},
    {6753, 1, &rule114},
    {6754, 1, &rule84},
    {6755, 2, &rule114},
    {6757, 8, &rule84},
    {6765, 6, &rule114},
    {6771, 10, &rule84},
    {6783, 1, &rule84},
    {6784, 10, &rule8},
    {6800, 10, &rule8},
    {6816, 7, &rule2},
    {6823, 1, &rule83},
    {6824, 6, &rule2},
    {6912, 4, &rule84},
    {6916, 1, &rule114},
    {6917, 47, &rule45},
    {6964, 1, &rule84},
    {6965, 1, &rule114},
    {6966, 5, &rule84},
    {6971, 1, &rule114},
    {6972, 1, &rule84},
    {6973, 5, &rule114},
    {6978, 1, &rule84},
    {6979, 2, &rule114},
    {6981, 7, &rule45},
    {6992, 10, &rule8},
    {7002, 7, &rule2},
    {7009, 10, &rule13},
    {7019, 9, &rule84},
    {7028, 9, &rule13},
    {7040, 2, &rule84},
    {7042, 1, &rule114},
    {7043, 30, &rule45},
    {7073, 1, &rule114},
    {7074, 4, &rule84},
    {7078, 2, &rule114},
    {7080, 2, &rule84},
    {7082, 1, &rule114},
    {7086, 2, &rule45},
    {7088, 10, &rule8},
    {7104, 38, &rule45},
    {7142, 1, &rule84},
    {7143, 1, &rule114},
    {7144, 2, &rule84},
    {7146, 3, &rule114},
    {7149, 1, &rule84},
    {7150, 1, &rule114},
    {7151, 3, &rule84},
    {7154, 2, &rule114},
    {7164, 4, &rule2},
    {7168, 36, &rule45},
    {7204, 8, &rule114},
    {7212, 8, &rule84},
    {7220, 2, &rule114},
    {7222, 2, &rule84},
    {7227, 5, &rule2},
    {7232, 10, &rule8},
    {7245, 3, &rule45},
    {7248, 10, &rule8},
    {7258, 30, &rule45},
    {7288, 6, &rule83},
    {7294, 2, &rule2},
    {7376, 3, &rule84},
    {7379, 1, &rule2},
    {7380, 13, &rule84},
    {7393, 1, &rule114},
    {7394, 7, &rule84},
    {7401, 4, &rule45},
    {7405, 1, &rule84},
    {7406, 4, &rule45},
    {7410, 1, &rule114},
    {7424, 44, &rule14},
    {7468, 54, &rule83},
    {7522, 22, &rule14},
    {7544, 1, &rule83},
    {7545, 1, &rule117},
    {7546, 3, &rule14},
    {7549, 1, &rule118},
    {7550, 29, &rule14},
    {7579, 37, &rule83},
    {7616, 39, &rule84},
    {7676, 4, &rule84},
    {7680, 1, &rule21},
    {7681, 1, &rule22},
    {7682, 1, &rule21},
    {7683, 1, &rule22},
    {7684, 1, &rule21},
    {7685, 1, &rule22},
    {7686, 1, &rule21},
    {7687, 1, &rule22},
    {7688, 1, &rule21},
    {7689, 1, &rule22},
    {7690, 1, &rule21},
    {7691, 1, &rule22},
    {7692, 1, &rule21},
    {7693, 1, &rule22},
    {7694, 1, &rule21},
    {7695, 1, &rule22},
    {7696, 1, &rule21},
    {7697, 1, &rule22},
    {7698, 1, &rule21},
    {7699, 1, &rule22},
    {7700, 1, &rule21},
    {7701, 1, &rule22},
    {7702, 1, &rule21},
    {7703, 1, &rule22},
    {7704, 1, &rule21},
    {7705, 1, &rule22},
    {7706, 1, &rule21},
    {7707, 1, &rule22},
    {7708, 1, &rule21},
    {7709, 1, &rule22},
    {7710, 1, &rule21},
    {7711, 1, &rule22},
    {7712, 1, &rule21},
    {7713, 1, &rule22},
    {7714, 1, &rule21},
    {7715, 1, &rule22},
    {7716, 1, &rule21},
    {7717, 1, &rule22},
    {7718, 1, &rule21},
    {7719, 1, &rule22},
    {7720, 1, &rule21},
    {7721, 1, &rule22},
    {7722, 1, &rule21},
    {7723, 1, &rule22},
    {7724, 1, &rule21},
    {7725, 1, &rule22},
    {7726, 1, &rule21},
    {7727, 1, &rule22},
    {7728, 1, &rule21},
    {7729, 1, &rule22},
    {7730, 1, &rule21},
    {7731, 1, &rule22},
    {7732, 1, &rule21},
    {7733, 1, &rule22},
    {7734, 1, &rule21},
    {7735, 1, &rule22},
    {7736, 1, &rule21},
    {7737, 1, &rule22},
    {7738, 1, &rule21},
    {7739, 1, &rule22},
    {7740, 1, &rule21},
    {7741, 1, &rule22},
    {7742, 1, &rule21},
    {7743, 1, &rule22},
    {7744, 1, &rule21},
    {7745, 1, &rule22},
    {7746, 1, &rule21},
    {7747, 1, &rule22},
    {7748, 1, &rule21},
    {7749, 1, &rule22},
    {7750, 1, &rule21},
    {7751, 1, &rule22},
    {7752, 1, &rule21},
    {7753, 1, &rule22},
    {7754, 1, &rule21},
    {7755, 1, &rule22},
    {7756, 1, &rule21},
    {7757, 1, &rule22},
    {7758, 1, &rule21},
    {7759, 1, &rule22},
    {7760, 1, &rule21},
    {7761, 1, &rule22},
    {7762, 1, &rule21},
    {7763, 1, &rule22},
    {7764, 1, &rule21},
    {7765, 1, &rule22},
    {7766, 1, &rule21},
    {7767, 1, &rule22},
    {7768, 1, &rule21},
    {7769, 1, &rule22},
    {7770, 1, &rule21},
    {7771, 1, &rule22},
    {7772, 1, &rule21},
    {7773, 1, &rule22},
    {7774, 1, &rule21},
    {7775, 1, &rule22},
    {7776, 1, &rule21},
    {7777, 1, &rule22},
    {7778, 1, &rule21},
    {7779, 1, &rule22},
    {7780, 1, &rule21},
    {7781, 1, &rule22},
    {7782, 1, &rule21},
    {7783, 1, &rule22},
    {7784, 1, &rule21},
    {7785, 1, &rule22},
    {7786, 1, &rule21},
    {7787, 1, &rule22},
    {7788, 1, &rule21},
    {7789, 1, &rule22},
    {7790, 1, &rule21},
    {7791, 1, &rule22},
    {7792, 1, &rule21},
    {7793, 1, &rule22},
    {7794, 1, &rule21},
    {7795, 1, &rule22},
    {7796, 1, &rule21},
    {7797, 1, &rule22},
    {7798, 1, &rule21},
    {7799, 1, &rule22},
    {7800, 1, &rule21},
    {7801, 1, &rule22},
    {7802, 1, &rule21},
    {7803, 1, &rule22},
    {7804, 1, &rule21},
    {7805, 1, &rule22},
    {7806, 1, &rule21},
    {7807, 1, &rule22},
    {7808, 1, &rule21},
    {7809, 1, &rule22},
    {7810, 1, &rule21},
    {7811, 1, &rule22},
    {7812, 1, &rule21},
    {7813, 1, &rule22},
    {7814, 1, &rule21},
    {7815, 1, &rule22},
    {7816, 1, &rule21},
    {7817, 1, &rule22},
    {7818, 1, &rule21},
    {7819, 1, &rule22},
    {7820, 1, &rule21},
    {7821, 1, &rule22},
    {7822, 1, &rule21},
    {7823, 1, &rule22},
    {7824, 1, &rule21},
    {7825, 1, &rule22},
    {7826, 1, &rule21},
    {7827, 1, &rule22},
    {7828, 1, &rule21},
    {7829, 1, &rule22},
    {7830, 5, &rule14},
    {7835, 1, &rule119},
    {7836, 2, &rule14},
    {7838, 1, &rule120},
    {7839, 1, &rule14},
    {7840, 1, &rule21},
    {7841, 1, &rule22},
    {7842, 1, &rule21},
    {7843, 1, &rule22},
    {7844, 1, &rule21},
    {7845, 1, &rule22},
    {7846, 1, &rule21},
    {7847, 1, &rule22},
    {7848, 1, &rule21},
    {7849, 1, &rule22},
    {7850, 1, &rule21},
    {7851, 1, &rule22},
    {7852, 1, &rule21},
    {7853, 1, &rule22},
    {7854, 1, &rule21},
    {7855, 1, &rule22},
    {7856, 1, &rule21},
    {7857, 1, &rule22},
    {7858, 1, &rule21},
    {7859, 1, &rule22},
    {7860, 1, &rule21},
    {7861, 1, &rule22},
    {7862, 1, &rule21},
    {7863, 1, &rule22},
    {7864, 1, &rule21},
    {7865, 1, &rule22},
    {7866, 1, &rule21},
    {7867, 1, &rule22},
    {7868, 1, &rule21},
    {7869, 1, &rule22},
    {7870, 1, &rule21},
    {7871, 1, &rule22},
    {7872, 1, &rule21},
    {7873, 1, &rule22},
    {7874, 1, &rule21},
    {7875, 1, &rule22},
    {7876, 1, &rule21},
    {7877, 1, &rule22},
    {7878, 1, &rule21},
    {7879, 1, &rule22},
    {7880, 1, &rule21},
    {7881, 1, &rule22},
    {7882, 1, &rule21},
    {7883, 1, &rule22},
    {7884, 1, &rule21},
    {7885, 1, &rule22},
    {7886, 1, &rule21},
    {7887, 1, &rule22},
    {7888, 1, &rule21},
    {7889, 1, &rule22},
    {7890, 1, &rule21},
    {7891, 1, &rule22},
    {7892, 1, &rule21},
    {7893, 1, &rule22},
    {7894, 1, &rule21},
    {7895, 1, &rule22},
    {7896, 1, &rule21},
    {7897, 1, &rule22},
    {7898, 1, &rule21},
    {7899, 1, &rule22},
    {7900, 1, &rule21},
    {7901, 1, &rule22},
    {7902, 1, &rule21},
    {7903, 1, &rule22},
    {7904, 1, &rule21},
    {7905, 1, &rule22},
    {7906, 1, &rule21},
    {7907, 1, &rule22},
    {7908, 1, &rule21},
    {7909, 1, &rule22},
    {7910, 1, &rule21},
    {7911, 1, &rule22},
    {7912, 1, &rule21},
    {7913, 1, &rule22},
    {7914, 1, &rule21},
    {7915, 1, &rule22},
    {7916, 1, &rule21},
    {7917, 1, &rule22},
    {7918, 1, &rule21},
    {7919, 1, &rule22},
    {7920, 1, &rule21},
    {7921, 1, &rule22},
    {7922, 1, &rule21},
    {7923, 1, &rule22},
    {7924, 1, &rule21},
    {7925, 1, &rule22},
    {7926, 1, &rule21},
    {7927, 1, &rule22},
    {7928, 1, &rule21},
    {7929, 1, &rule22},
    {7930, 1, &rule21},
    {7931, 1, &rule22},
    {7932, 1, &rule21},
    {7933, 1, &rule22},
    {7934, 1, &rule21},
    {7935, 1, &rule22},
    {7936, 8, &rule121},
    {7944, 8, &rule122},
    {7952, 6, &rule121},
    {7960, 6, &rule122},
    {7968, 8, &rule121},
    {7976, 8, &rule122},
    {7984, 8, &rule121},
    {7992, 8, &rule122},
    {8000, 6, &rule121},
    {8008, 6, &rule122},
    {8016, 1, &rule14},
    {8017, 1, &rule121},
    {8018, 1, &rule14},
    {8019, 1, &rule121},
    {8020, 1, &rule14},
    {8021, 1, &rule121},
    {8022, 1, &rule14},
    {8023, 1, &rule121},
    {8025, 1, &rule122},
    {8027, 1, &rule122},
    {8029, 1, &rule122},
    {8031, 1, &rule122},
    {8032, 8, &rule121},
    {8040, 8, &rule122},
    {8048, 2, &rule123},
    {8050, 4, &rule124},
    {8054, 2, &rule125},
    {8056, 2, &rule126},
    {8058, 2, &rule127},
    {8060, 2, &rule128},
    {8064, 8, &rule121},
    {8072, 8, &rule129},
    {8080, 8, &rule121},
    {8088, 8, &rule129},
    {8096, 8, &rule121},
    {8104, 8, &rule129},
    {8112, 2, &rule121},
    {8114, 1, &rule14},
    {8115, 1, &rule130},
    {8116, 1, &rule14},
    {8118, 2, &rule14},
    {8120, 2, &rule122},
    {8122, 2, &rule131},
    {8124, 1, &rule132},
    {8125, 1, &rule10},
    {8126, 1, &rule133},
    {8127, 3, &rule10},
    {8130, 1, &rule14},
    {8131, 1, &rule130},
    {8132, 1, &rule14},
    {8134, 2, &rule14},
    {8136, 4, &rule134},
    {8140, 1, &rule132},
    {8141, 3, &rule10},
    {8144, 2, &rule121},
    {8146, 2, &rule14},
    {8150, 2, &rule14},
    {8152, 2, &rule122},
    {8154, 2, &rule135},
    {8157, 3, &rule10},
    {8160, 2, &rule121},
    {8162, 3, &rule14},
    {8165, 1, &rule104},
    {8166, 2, &rule14},
    {8168, 2, &rule122},
    {8170, 2, &rule136},
    {8172, 1, &rule107},
    {8173, 3, &rule10},
    {8178, 1, &rule14},
    {8179, 1, &rule130},
    {8180, 1, &rule14},
    {8182, 2, &rule14},
    {8184, 2, &rule137},
    {8186, 2, &rule138},
    {8188, 1, &rule132},
    {8189, 2, &rule10},
    {8192, 11, &rule1},
    {8203, 5, &rule16},
    {8208, 6, &rule7},
    {8214, 2, &rule2},
    {8216, 1, &rule15},
    {8217, 1, &rule19},
    {8218, 1, &rule4},
    {8219, 2, &rule15},
    {8221, 1, &rule19},
    {8222, 1, &rule4},
    {8223, 1, &rule15},
    {8224, 8, &rule2},
    {8232, 1, &rule139},
    {8233, 1, &rule140},
    {8234, 5, &rule16},
    {8239, 1, &rule1},
    {8240, 9, &rule2},
    {8249, 1, &rule15},
    {8250, 1, &rule19},
    {8251, 4, &rule2},
    {8255, 2, &rule11},
    {8257, 3, &rule2},
    {8260, 1, &rule6},
    {8261, 1, &rule4},
    {8262, 1, &rule5},
    {8263, 11, &rule2},
    {8274, 1, &rule6},
    {8275, 1, &rule2},
    {8276, 1, &rule11},
    {8277, 10, &rule2},
    {8287, 1, &rule1},
    {8288, 5, &rule16},
    {8298, 6, &rule16},
    {8304, 1, &rule17},
    {8305, 1, &rule83},
    {8308, 6, &rule17},
    {8314, 3, &rule6},
    {8317, 1, &rule4},
    {8318, 1, &rule5},
    {8319, 1, &rule83},
    {8320, 10, &rule17},
    {8330, 3, &rule6},
    {8333, 1, &rule4},
    {8334, 1, &rule5},
    {8336, 13, &rule83},
    {8352, 26, &rule3},
    {8400, 13, &rule84},
    {8413, 4, &rule109},
    {8417, 1, &rule84},
    {8418, 3, &rule109},
    {8421, 12, &rule84},
    {8448, 2, &rule13},
    {8450, 1, &rule98},
    {8451, 4, &rule13},
    {8455, 1, &rule98},
    {8456, 2, &rule13},
    {8458, 1, &rule14},
    {8459, 3, &rule98},
    {8462, 2, &rule14},
    {8464, 3, &rule98},
    {8467, 1, &rule14},
    {8468, 1, &rule13},
    {8469, 1, &rule98},
    {8470, 2, &rule13},
    {8472, 1, &rule6},
    {8473, 5, &rule98},
    {8478, 6, &rule13},
    {8484, 1, &rule98},
    {8485, 1, &rule13},
    {8486, 1, &rule141},
    {8487, 1, &rule13},
    {8488, 1, &rule98},
    {8489, 1, &rule13},
    {8490, 1, &rule142},
    {8491, 1, &rule143},
    {8492, 2, &rule98},
    {8494, 1, &rule13},
    {8495, 1, &rule14},
    {8496, 2, &rule98},
    {8498, 1, &rule144},
    {8499, 1, &rule98},
    {8500, 1, &rule14},
    {8501, 4, &rule45},
    {8505, 1, &rule14},
    {8506, 2, &rule13},
    {8508, 2, &rule14},
    {8510, 2, &rule98},
    {8512, 5, &rule6},
    {8517, 1, &rule98},
    {8518, 4, &rule14},
    {8522, 1, &rule13},
    {8523, 1, &rule6},
    {8524, 2, &rule13},
    {8526, 1, &rule145},
    {8527, 1, &rule13},
    {8528, 16, &rule17},
    {8544, 16, &rule146},
    {8560, 16, &rule147},
    {8576, 3, &rule116},
    {8579, 1, &rule21},
    {8580, 1, &rule22},
    {8581, 4, &rule116},
    {8585, 1, &rule17},
    {8592, 5, &rule6},
    {8597, 5, &rule13},
    {8602, 2, &rule6},
    {8604, 4, &rule13},
    {8608, 1, &rule6},
    {8609, 2, &rule13},
    {8611, 1, &rule6},
    {8612, 2, &rule13},
    {8614, 1, &rule6},
    {8615, 7, &rule13},
    {8622, 1, &rule6},
    {8623, 31, &rule13},
    {8654, 2, &rule6},
    {8656, 2, &rule13},
    {8658, 1, &rule6},
    {8659, 1, &rule13},
    {8660, 1, &rule6},
    {8661, 31, &rule13},
    {8692, 268, &rule6},
    {8960, 8, &rule13},
    {8968, 4, &rule6},
    {8972, 20, &rule13},
    {8992, 2, &rule6},
    {8994, 7, &rule13},
    {9001, 1, &rule4},
    {9002, 1, &rule5},
    {9003, 81, &rule13},
    {9084, 1, &rule6},
    {9085, 30, &rule13},
    {9115, 25, &rule6},
    {9140, 40, &rule13},
    {9180, 6, &rule6},
    {9186, 18, &rule13},
    {9216, 39, &rule13},
    {9280, 11, &rule13},
    {9312, 60, &rule17},
    {9372, 26, &rule13},
    {9398, 26, &rule148},
    {9424, 26, &rule149},
    {9450, 22, &rule17},
    {9472, 183, &rule13},
    {9655, 1, &rule6},
    {9656, 9, &rule13},
    {9665, 1, &rule6},
    {9666, 54, &rule13},
    {9720, 8, &rule6},
    {9728, 111, &rule13},
    {9839, 1, &rule6},
    {9840, 144, &rule13},
    {9985, 103, &rule13},
    {10088, 1, &rule4},
    {10089, 1, &rule5},
    {10090, 1, &rule4},
    {10091, 1, &rule5},
    {10092, 1, &rule4},
    {10093, 1, &rule5},
    {10094, 1, &rule4},
    {10095, 1, &rule5},
    {10096, 1, &rule4},
    {10097, 1, &rule5},
    {10098, 1, &rule4},
    {10099, 1, &rule5},
    {10100, 1, &rule4},
    {10101, 1, &rule5},
    {10102, 30, &rule17},
    {10132, 44, &rule13},
    {10176, 5, &rule6},
    {10181, 1, &rule4},
    {10182, 1, &rule5},
    {10183, 4, &rule6},
    {10188, 1, &rule6},
    {10190, 24, &rule6},
    {10214, 1, &rule4},
    {10215, 1, &rule5},
    {10216, 1, &rule4},
    {10217, 1, &rule5},
    {10218, 1, &rule4},
    {10219, 1, &rule5},
    {10220, 1, &rule4},
    {10221, 1, &rule5},
    {10222, 1, &rule4},
    {10223, 1, &rule5},
    {10224, 16, &rule6},
    {10240, 256, &rule13},
    {10496, 131, &rule6},
    {10627, 1, &rule4},
    {10628, 1, &rule5},
    {10629, 1, &rule4},
    {10630, 1, &rule5},
    {10631, 1, &rule4},
    {10632, 1, &rule5},
    {10633, 1, &rule4},
    {10634, 1, &rule5},
    {10635, 1, &rule4},
    {10636, 1, &rule5},
    {10637, 1, &rule4},
    {10638, 1, &rule5},
    {10639, 1, &rule4},
    {10640, 1, &rule5},
    {10641, 1, &rule4},
    {10642, 1, &rule5},
    {10643, 1, &rule4},
    {10644, 1, &rule5},
    {10645, 1, &rule4},
    {10646, 1, &rule5},
    {10647, 1, &rule4},
    {10648, 1, &rule5},
    {10649, 63, &rule6},
    {10712, 1, &rule4},
    {10713, 1, &rule5},
    {10714, 1, &rule4},
    {10715, 1, &rule5},
    {10716, 32, &rule6},
    {10748, 1, &rule4},
    {10749, 1, &rule5},
    {10750, 258, &rule6},
    {11008, 48, &rule13},
    {11056, 21, &rule6},
    {11077, 2, &rule13},
    {11079, 6, &rule6},
    {11088, 10, &rule13},
    {11264, 47, &rule112},
    {11312, 47, &rule113},
    {11360, 1, &rule21},
    {11361, 1, &rule22},
    {11362, 1, &rule150},
    {11363, 1, &rule151},
    {11364, 1, &rule152},
    {11365, 1, &rule153},
    {11366, 1, &rule154},
    {11367, 1, &rule21},
    {11368, 1, &rule22},
    {11369, 1, &rule21},
    {11370, 1, &rule22},
    {11371, 1, &rule21},
    {11372, 1, &rule22},
    {11373, 1, &rule155},
    {11374, 1, &rule156},
    {11375, 1, &rule157},
    {11376, 1, &rule158},
    {11377, 1, &rule14},
    {11378, 1, &rule21},
    {11379, 1, &rule22},
    {11380, 1, &rule14},
    {11381, 1, &rule21},
    {11382, 1, &rule22},
    {11383, 6, &rule14},
    {11389, 1, &rule83},
    {11390, 2, &rule159},
    {11392, 1, &rule21},
    {11393, 1, &rule22},
    {11394, 1, &rule21},
    {11395, 1, &rule22},
    {11396, 1, &rule21},
    {11397, 1, &rule22},
    {11398, 1, &rule21},
    {11399, 1, &rule22},
    {11400, 1, &rule21},
    {11401, 1, &rule22},
    {11402, 1, &rule21},
    {11403, 1, &rule22},
    {11404, 1, &rule21},
    {11405, 1, &rule22},
    {11406, 1, &rule21},
    {11407, 1, &rule22},
    {11408, 1, &rule21},
    {11409, 1, &rule22},
    {11410, 1, &rule21},
    {11411, 1, &rule22},
    {11412, 1, &rule21},
    {11413, 1, &rule22},
    {11414, 1, &rule21},
    {11415, 1, &rule22},
    {11416, 1, &rule21},
    {11417, 1, &rule22},
    {11418, 1, &rule21},
    {11419, 1, &rule22},
    {11420, 1, &rule21},
    {11421, 1, &rule22},
    {11422, 1, &rule21},
    {11423, 1, &rule22},
    {11424, 1, &rule21},
    {11425, 1, &rule22},
    {11426, 1, &rule21},
    {11427, 1, &rule22},
    {11428, 1, &rule21},
    {11429, 1, &rule22},
    {11430, 1, &rule21},
    {11431, 1, &rule22},
    {11432, 1, &rule21},
    {11433, 1, &rule22},
    {11434, 1, &rule21},
    {11435, 1, &rule22},
    {11436, 1, &rule21},
    {11437, 1, &rule22},
    {11438, 1, &rule21},
    {11439, 1, &rule22},
    {11440, 1, &rule21},
    {11441, 1, &rule22},
    {11442, 1, &rule21},
    {11443, 1, &rule22},
    {11444, 1, &rule21},
    {11445, 1, &rule22},
    {11446, 1, &rule21},
    {11447, 1, &rule22},
    {11448, 1, &rule21},
    {11449, 1, &rule22},
    {11450, 1, &rule21},
    {11451, 1, &rule22},
    {11452, 1, &rule21},
    {11453, 1, &rule22},
    {11454, 1, &rule21},
    {11455, 1, &rule22},
    {11456, 1, &rule21},
    {11457, 1, &rule22},
    {11458, 1, &rule21},
    {11459, 1, &rule22},
    {11460, 1, &rule21},
    {11461, 1, &rule22},
    {11462, 1, &rule21},
    {11463, 1, &rule22},
    {11464, 1, &rule21},
    {11465, 1, &rule22},
    {11466, 1, &rule21},
    {11467, 1, &rule22},
    {11468, 1, &rule21},
    {11469, 1, &rule22},
    {11470, 1, &rule21},
    {11471, 1, &rule22},
    {11472, 1, &rule21},
    {11473, 1, &rule22},
    {11474, 1, &rule21},
    {11475, 1, &rule22},
    {11476, 1, &rule21},
    {11477, 1, &rule22},
    {11478, 1, &rule21},
    {11479, 1, &rule22},
    {11480, 1, &rule21},
    {11481, 1, &rule22},
    {11482, 1, &rule21},
    {11483, 1, &rule22},
    {11484, 1, &rule21},
    {11485, 1, &rule22},
    {11486, 1, &rule21},
    {11487, 1, &rule22},
    {11488, 1, &rule21},
    {11489, 1, &rule22},
    {11490, 1, &rule21},
    {11491, 1, &rule22},
    {11492, 1, &rule14},
    {11493, 6, &rule13},
    {11499, 1, &rule21},
    {11500, 1, &rule22},
    {11501, 1, &rule21},
    {11502, 1, &rule22},
    {11503, 3, &rule84},
    {11513, 4, &rule2},
    {11517, 1, &rule17},
    {11518, 2, &rule2},
    {11520, 38, &rule160},
    {11568, 54, &rule45},
    {11631, 1, &rule83},
    {11632, 1, &rule2},
    {11647, 1, &rule84},
    {11648, 23, &rule45},
    {11680, 7, &rule45},
    {11688, 7, &rule45},
    {11696, 7, &rule45},
    {11704, 7, &rule45},
    {11712, 7, &rule45},
    {11720, 7, &rule45},
    {11728, 7, &rule45},
    {11736, 7, &rule45},
    {11744, 32, &rule84},
    {11776, 2, &rule2},
    {11778, 1, &rule15},
    {11779, 1, &rule19},
    {11780, 1, &rule15},
    {11781, 1, &rule19},
    {11782, 3, &rule2},
    {11785, 1, &rule15},
    {11786, 1, &rule19},
    {11787, 1, &rule2},
    {11788, 1, &rule15},
    {11789, 1, &rule19},
    {11790, 9, &rule2},
    {11799, 1, &rule7},
    {11800, 2, &rule2},
    {11802, 1, &rule7},
    {11803, 1, &rule2},
    {11804, 1, &rule15},
    {11805, 1, &rule19},
    {11806, 2, &rule2},
    {11808, 1, &rule15},
    {11809, 1, &rule19},
    {11810, 1, &rule4},
    {11811, 1, &rule5},
    {11812, 1, &rule4},
    {11813, 1, &rule5},
    {11814, 1, &rule4},
    {11815, 1, &rule5},
    {11816, 1, &rule4},
    {11817, 1, &rule5},
    {11818, 5, &rule2},
    {11823, 1, &rule83},
    {11824, 2, &rule2},
    {11904, 26, &rule13},
    {11931, 89, &rule13},
    {12032, 214, &rule13},
    {12272, 12, &rule13},
    {12288, 1, &rule1},
    {12289, 3, &rule2},
    {12292, 1, &rule13},
    {12293, 1, &rule83},
    {12294, 1, &rule45},
    {12295, 1, &rule116},
    {12296, 1, &rule4},
    {12297, 1, &rule5},
    {12298, 1, &rule4},
    {12299, 1, &rule5},
    {12300, 1, &rule4},
    {12301, 1, &rule5},
    {12302, 1, &rule4},
    {12303, 1, &rule5},
    {12304, 1, &rule4},
    {12305, 1, &rule5},
    {12306, 2, &rule13},
    {12308, 1, &rule4},
    {12309, 1, &rule5},
    {12310, 1, &rule4},
    {12311, 1, &rule5},
    {12312, 1, &rule4},
    {12313, 1, &rule5},
    {12314, 1, &rule4},
    {12315, 1, &rule5},
    {12316, 1, &rule7},
    {12317, 1, &rule4},
    {12318, 2, &rule5},
    {12320, 1, &rule13},
    {12321, 9, &rule116},
    {12330, 6, &rule84},
    {12336, 1, &rule7},
    {12337, 5, &rule83},
    {12342, 2, &rule13},
    {12344, 3, &rule116},
    {12347, 1, &rule83},
    {12348, 1, &rule45},
    {12349, 1, &rule2},
    {12350, 2, &rule13},
    {12353, 86, &rule45},
    {12441, 2, &rule84},
    {12443, 2, &rule10},
    {12445, 2, &rule83},
    {12447, 1, &rule45},
    {12448, 1, &rule7},
    {12449, 90, &rule45},
    {12539, 1, &rule2},
    {12540, 3, &rule83},
    {12543, 1, &rule45},
    {12549, 41, &rule45},
    {12593, 94, &rule45},
    {12688, 2, &rule13},
    {12690, 4, &rule17},
    {12694, 10, &rule13},
    {12704, 27, &rule45},
    {12736, 36, &rule13},
    {12784, 16, &rule45},
    {12800, 31, &rule13},
    {12832, 10, &rule17},
    {12842, 39, &rule13},
    {12881, 15, &rule17},
    {12896, 32, &rule13},
    {12928, 10, &rule17},
    {12938, 39, &rule13},
    {12977, 15, &rule17},
    {12992, 63, &rule13},
    {13056, 256, &rule13},
    {13312, 6582, &rule45},
    {19904, 64, &rule13},
    {19968, 20940, &rule45},
    {40960, 21, &rule45},
    {40981, 1, &rule83},
    {40982, 1143, &rule45},
    {42128, 55, &rule13},
    {42192, 40, &rule45},
    {42232, 6, &rule83},
    {42238, 2, &rule2},
    {42240, 268, &rule45},
    {42508, 1, &rule83},
    {42509, 3, &rule2},
    {42512, 16, &rule45},
    {42528, 10, &rule8},
    {42538, 2, &rule45},
    {42560, 1, &rule21},
    {42561, 1, &rule22},
    {42562, 1, &rule21},
    {42563, 1, &rule22},
    {42564, 1, &rule21},
    {42565, 1, &rule22},
    {42566, 1, &rule21},
    {42567, 1, &rule22},
    {42568, 1, &rule21},
    {42569, 1, &rule22},
    {42570, 1, &rule21},
    {42571, 1, &rule22},
    {42572, 1, &rule21},
    {42573, 1, &rule22},
    {42574, 1, &rule21},
    {42575, 1, &rule22},
    {42576, 1, &rule21},
    {42577, 1, &rule22},
    {42578, 1, &rule21},
    {42579, 1, &rule22},
    {42580, 1, &rule21},
    {42581, 1, &rule22},
    {42582, 1, &rule21},
    {42583, 1, &rule22},
    {42584, 1, &rule21},
    {42585, 1, &rule22},
    {42586, 1, &rule21},
    {42587, 1, &rule22},
    {42588, 1, &rule21},
    {42589, 1, &rule22},
    {42590, 1, &rule21},
    {42591, 1, &rule22},
    {42592, 1, &rule21},
    {42593, 1, &rule22},
    {42594, 1, &rule21},
    {42595, 1, &rule22},
    {42596, 1, &rule21},
    {42597, 1, &rule22},
    {42598, 1, &rule21},
    {42599, 1, &rule22},
    {42600, 1, &rule21},
    {42601, 1, &rule22},
    {42602, 1, &rule21},
    {42603, 1, &rule22},
    {42604, 1, &rule21},
    {42605, 1, &rule22},
    {42606, 1, &rule45},
    {42607, 1, &rule84},
    {42608, 3, &rule109},
    {42611, 1, &rule2},
    {42620, 2, &rule84},
    {42622, 1, &rule2},
    {42623, 1, &rule83},
    {42624, 1, &rule21},
    {42625, 1, &rule22},
    {42626, 1, &rule21},
    {42627, 1, &rule22},
    {42628, 1, &rule21},
    {42629, 1, &rule22},
    {42630, 1, &rule21},
    {42631, 1, &rule22},
    {42632, 1, &rule21},
    {42633, 1, &rule22},
    {42634, 1, &rule21},
    {42635, 1, &rule22},
    {42636, 1, &rule21},
    {42637, 1, &rule22},
    {42638, 1, &rule21},
    {42639, 1, &rule22},
    {42640, 1, &rule21},
    {42641, 1, &rule22},
    {42642, 1, &rule21},
    {42643, 1, &rule22},
    {42644, 1, &rule21},
    {42645, 1, &rule22},
    {42646, 1, &rule21},
    {42647, 1, &rule22},
    {42656, 70, &rule45},
    {42726, 10, &rule116},
    {42736, 2, &rule84},
    {42738, 6, &rule2},
    {42752, 23, &rule10},
    {42775, 9, &rule83},
    {42784, 2, &rule10},
    {42786, 1, &rule21},
    {42787, 1, &rule22},
    {42788, 1, &rule21},
    {42789, 1, &rule22},
    {42790, 1, &rule21},
    {42791, 1, &rule22},
    {42792, 1, &rule21},
    {42793, 1, &rule22},
    {42794, 1, &rule21},
    {42795, 1, &rule22},
    {42796, 1, &rule21},
    {42797, 1, &rule22},
    {42798, 1, &rule21},
    {42799, 1, &rule22},
    {42800, 2, &rule14},
    {42802, 1, &rule21},
    {42803, 1, &rule22},
    {42804, 1, &rule21},
    {42805, 1, &rule22},
    {42806, 1, &rule21},
    {42807, 1, &rule22},
    {42808, 1, &rule21},
    {42809, 1, &rule22},
    {42810, 1, &rule21},
    {42811, 1, &rule22},
    {42812, 1, &rule21},
    {42813, 1, &rule22},
    {42814, 1, &rule21},
    {42815, 1, &rule22},
    {42816, 1, &rule21},
    {42817, 1, &rule22},
    {42818, 1, &rule21},
    {42819, 1, &rule22},
    {42820, 1, &rule21},
    {42821, 1, &rule22},
    {42822, 1, &rule21},
    {42823, 1, &rule22},
    {42824, 1, &rule21},
    {42825, 1, &rule22},
    {42826, 1, &rule21},
    {42827, 1, &rule22},
    {42828, 1, &rule21},
    {42829, 1, &rule22},
    {42830, 1, &rule21},
    {42831, 1, &rule22},
    {42832, 1, &rule21},
    {42833, 1, &rule22},
    {42834, 1, &rule21},
    {42835, 1, &rule22},
    {42836, 1, &rule21},
    {42837, 1, &rule22},
    {42838, 1, &rule21},
    {42839, 1, &rule22},
    {42840, 1, &rule21},
    {42841, 1, &rule22},
    {42842, 1, &rule21},
    {42843, 1, &rule22},
    {42844, 1, &rule21},
    {42845, 1, &rule22},
    {42846, 1, &rule21},
    {42847, 1, &rule22},
    {42848, 1, &rule21},
    {42849, 1, &rule22},
    {42850, 1, &rule21},
    {42851, 1, &rule22},
    {42852, 1, &rule21},
    {42853, 1, &rule22},
    {42854, 1, &rule21},
    {42855, 1, &rule22},
    {42856, 1, &rule21},
    {42857, 1, &rule22},
    {42858, 1, &rule21},
    {42859, 1, &rule22},
    {42860, 1, &rule21},
    {42861, 1, &rule22},
    {42862, 1, &rule21},
    {42863, 1, &rule22},
    {42864, 1, &rule83},
    {42865, 8, &rule14},
    {42873, 1, &rule21},
    {42874, 1, &rule22},
    {42875, 1, &rule21},
    {42876, 1, &rule22},
    {42877, 1, &rule161},
    {42878, 1, &rule21},
    {42879, 1, &rule22},
    {42880, 1, &rule21},
    {42881, 1, &rule22},
    {42882, 1, &rule21},
    {42883, 1, &rule22},
    {42884, 1, &rule21},
    {42885, 1, &rule22},
    {42886, 1, &rule21},
    {42887, 1, &rule22},
    {42888, 1, &rule83},
    {42889, 2, &rule10},
    {42891, 1, &rule21},
    {42892, 1, &rule22},
    {42893, 1, &rule162},
    {42894, 1, &rule14},
    {42896, 1, &rule21},
    {42897, 1, &rule22},
    {42912, 1, &rule21},
    {42913, 1, &rule22},
    {42914, 1, &rule21},
    {42915, 1, &rule22},
    {42916, 1, &rule21},
    {42917, 1, &rule22},
    {42918, 1, &rule21},
    {42919, 1, &rule22},
    {42920, 1, &rule21},
    {42921, 1, &rule22},
    {43002, 1, &rule14},
    {43003, 7, &rule45},
    {43010, 1, &rule84},
    {43011, 3, &rule45},
    {43014, 1, &rule84},
    {43015, 4, &rule45},
    {43019, 1, &rule84},
    {43020, 23, &rule45},
    {43043, 2, &rule114},
    {43045, 2, &rule84},
    {43047, 1, &rule114},
    {43048, 4, &rule13},
    {43056, 6, &rule17},
    {43062, 2, &rule13},
    {43064, 1, &rule3},
    {43065, 1, &rule13},
    {43072, 52, &rule45},
    {43124, 4, &rule2},
    {43136, 2, &rule114},
    {43138, 50, &rule45},
    {43188, 16, &rule114},
    {43204, 1, &rule84},
    {43214, 2, &rule2},
    {43216, 10, &rule8},
    {43232, 18, &rule84},
    {43250, 6, &rule45},
    {43256, 3, &rule2},
    {43259, 1, &rule45},
    {43264, 10, &rule8},
    {43274, 28, &rule45},
    {43302, 8, &rule84},
    {43310, 2, &rule2},
    {43312, 23, &rule45},
    {43335, 11, &rule84},
    {43346, 2, &rule114},
    {43359, 1, &rule2},
    {43360, 29, &rule45},
    {43392, 3, &rule84},
    {43395, 1, &rule114},
    {43396, 47, &rule45},
    {43443, 1, &rule84},
    {43444, 2, &rule114},
    {43446, 4, &rule84},
    {43450, 2, &rule114},
    {43452, 1, &rule84},
    {43453, 4, &rule114},
    {43457, 13, &rule2},
    {43471, 1, &rule83},
    {43472, 10, &rule8},
    {43486, 2, &rule2},
    {43520, 41, &rule45},
    {43561, 6, &rule84},
    {43567, 2, &rule114},
    {43569, 2, &rule84},
    {43571, 2, &rule114},
    {43573, 2, &rule84},
    {43584, 3, &rule45},
    {43587, 1, &rule84},
    {43588, 8, &rule45},
    {43596, 1, &rule84},
    {43597, 1, &rule114},
    {43600, 10, &rule8},
    {43612, 4, &rule2},
    {43616, 16, &rule45},
    {43632, 1, &rule83},
    {43633, 6, &rule45},
    {43639, 3, &rule13},
    {43642, 1, &rule45},
    {43643, 1, &rule114},
    {43648, 48, &rule45},
    {43696, 1, &rule84},
    {43697, 1, &rule45},
    {43698, 3, &rule84},
    {43701, 2, &rule45},
    {43703, 2, &rule84},
    {43705, 5, &rule45},
    {43710, 2, &rule84},
    {43712, 1, &rule45},
    {43713, 1, &rule84},
    {43714, 1, &rule45},
    {43739, 2, &rule45},
    {43741, 1, &rule83},
    {43742, 2, &rule2},
    {43777, 6, &rule45},
    {43785, 6, &rule45},
    {43793, 6, &rule45},
    {43808, 7, &rule45},
    {43816, 7, &rule45},
    {43968, 35, &rule45},
    {44003, 2, &rule114},
    {44005, 1, &rule84},
    {44006, 2, &rule114},
    {44008, 1, &rule84},
    {44009, 2, &rule114},
    {44011, 1, &rule2},
    {44012, 1, &rule114},
    {44013, 1, &rule84},
    {44016, 10, &rule8},
    {44032, 11172, &rule45},
    {55216, 23, &rule45},
    {55243, 49, &rule45},
    {55296, 896, &rule163},
    {56192, 128, &rule163},
    {56320, 1024, &rule163},
    {57344, 6400, &rule164},
    {63744, 302, &rule45},
    {64048, 62, &rule45},
    {64112, 106, &rule45},
    {64256, 7, &rule14},
    {64275, 5, &rule14},
    {64285, 1, &rule45},
    {64286, 1, &rule84},
    {64287, 10, &rule45},
    {64297, 1, &rule6},
    {64298, 13, &rule45},
    {64312, 5, &rule45},
    {64318, 1, &rule45},
    {64320, 2, &rule45},
    {64323, 2, &rule45},
    {64326, 108, &rule45},
    {64434, 16, &rule10},
    {64467, 363, &rule45},
    {64830, 1, &rule4},
    {64831, 1, &rule5},
    {64848, 64, &rule45},
    {64914, 54, &rule45},
    {65008, 12, &rule45},
    {65020, 1, &rule3},
    {65021, 1, &rule13},
    {65024, 16, &rule84},
    {65040, 7, &rule2},
    {65047, 1, &rule4},
    {65048, 1, &rule5},
    {65049, 1, &rule2},
    {65056, 7, &rule84},
    {65072, 1, &rule2},
    {65073, 2, &rule7},
    {65075, 2, &rule11},
    {65077, 1, &rule4},
    {65078, 1, &rule5},
    {65079, 1, &rule4},
    {65080, 1, &rule5},
    {65081, 1, &rule4},
    {65082, 1, &rule5},
    {65083, 1, &rule4},
    {65084, 1, &rule5},
    {65085, 1, &rule4},
    {65086, 1, &rule5},
    {65087, 1, &rule4},
    {65088, 1, &rule5},
    {65089, 1, &rule4},
    {65090, 1, &rule5},
    {65091, 1, &rule4},
    {65092, 1, &rule5},
    {65093, 2, &rule2},
    {65095, 1, &rule4},
    {65096, 1, &rule5},
    {65097, 4, &rule2},
    {65101, 3, &rule11},
    {65104, 3, &rule2},
    {65108, 4, &rule2},
    {65112, 1, &rule7},
    {65113, 1, &rule4},
    {65114, 1, &rule5},
    {65115, 1, &rule4},
    {65116, 1, &rule5},
    {65117, 1, &rule4},
    {65118, 1, &rule5},
    {65119, 3, &rule2},
    {65122, 1, &rule6},
    {65123, 1, &rule7},
    {65124, 3, &rule6},
    {65128, 1, &rule2},
    {65129, 1, &rule3},
    {65130, 2, &rule2},
    {65136, 5, &rule45},
    {65142, 135, &rule45},
    {65279, 1, &rule16},
    {65281, 3, &rule2},
    {65284, 1, &rule3},
    {65285, 3, &rule2},
    {65288, 1, &rule4},
    {65289, 1, &rule5},
    {65290, 1, &rule2},
    {65291, 1, &rule6},
    {65292, 1, &rule2},
    {65293, 1, &rule7},
    {65294, 2, &rule2},
    {65296, 10, &rule8},
    {65306, 2, &rule2},
    {65308, 3, &rule6},
    {65311, 2, &rule2},
    {65313, 26, &rule9},
    {65339, 1, &rule4},
    {65340, 1, &rule2},
    {65341, 1, &rule5},
    {65342, 1, &rule10},
    {65343, 1, &rule11},
    {65344, 1, &rule10},
    {65345, 26, &rule12},
    {65371, 1, &rule4},
    {65372, 1, &rule6},
    {65373, 1, &rule5},
    {65374, 1, &rule6},
    {65375, 1, &rule4},
    {65376, 1, &rule5},
    {65377, 1, &rule2},
    {65378, 1, &rule4},
    {65379, 1, &rule5},
    {65380, 2, &rule2},
    {65382, 10, &rule45},
    {65392, 1, &rule83},
    {65393, 45, &rule45},
    {65438, 2, &rule83},
    {65440, 31, &rule45},
    {65474, 6, &rule45},
    {65482, 6, &rule45},
    {65490, 6, &rule45},
    {65498, 3, &rule45},
    {65504, 2, &rule3},
    {65506, 1, &rule6},
    {65507, 1, &rule10},
    {65508, 1, &rule13},
    {65509, 2, &rule3},
    {65512, 1, &rule13},
    {65513, 4, &rule6},
    {65517, 2, &rule13},
    {65529, 3, &rule16},
    {65532, 2, &rule13},
    {65536, 12, &rule45},
    {65549, 26, &rule45},
    {65576, 19, &rule45},
    {65596, 2, &rule45},
    {65599, 15, &rule45},
    {65616, 14, &rule45},
    {65664, 123, &rule45},
    {65792, 2, &rule2},
    {65794, 1, &rule13},
    {65799, 45, &rule17},
    {65847, 9, &rule13},
    {65856, 53, &rule116},
    {65909, 4, &rule17},
    {65913, 17, &rule13},
    {65930, 1, &rule17},
    {65936, 12, &rule13},
    {66000, 45, &rule13},
    {66045, 1, &rule84},
    {66176, 29, &rule45},
    {66208, 49, &rule45},
    {66304, 31, &rule45},
    {66336, 4, &rule17},
    {66352, 17, &rule45},
    {66369, 1, &rule116},
    {66370, 8, &rule45},
    {66378, 1, &rule116},
    {66432, 30, &rule45},
    {66463, 1, &rule2},
    {66464, 36, &rule45},
    {66504, 8, &rule45},
    {66512, 1, &rule2},
    {66513, 5, &rule116},
    {66560, 40, &rule165},
    {66600, 40, &rule166},
    {66640, 78, &rule45},
    {66720, 10, &rule8},
    {67584, 6, &rule45},
    {67592, 1, &rule45},
    {67594, 44, &rule45},
    {67639, 2, &rule45},
    {67644, 1, &rule45},
    {67647, 23, &rule45},
    {67671, 1, &rule2},
    {67672, 8, &rule17},
    {67840, 22, &rule45},
    {67862, 6, &rule17},
    {67871, 1, &rule2},
    {67872, 26, &rule45},
    {67903, 1, &rule2},
    {68096, 1, &rule45},
    {68097, 3, &rule84},
    {68101, 2, &rule84},
    {68108, 4, &rule84},
    {68112, 4, &rule45},
    {68117, 3, &rule45},
    {68121, 27, &rule45},
    {68152, 3, &rule84},
    {68159, 1, &rule84},
    {68160, 8, &rule17},
    {68176, 9, &rule2},
    {68192, 29, &rule45},
    {68221, 2, &rule17},
    {68223, 1, &rule2},
    {68352, 54, &rule45},
    {68409, 7, &rule2},
    {68416, 22, &rule45},
    {68440, 8, &rule17},
    {68448, 19, &rule45},
    {68472, 8, &rule17},
    {68608, 73, &rule45},
    {69216, 31, &rule17},
    {69632, 1, &rule114},
    {69633, 1, &rule84},
    {69634, 1, &rule114},
    {69635, 53, &rule45},
    {69688, 15, &rule84},
    {69703, 7, &rule2},
    {69714, 20, &rule17},
    {69734, 10, &rule8},
    {69760, 2, &rule84},
    {69762, 1, &rule114},
    {69763, 45, &rule45},
    {69808, 3, &rule114},
    {69811, 4, &rule84},
    {69815, 2, &rule114},
    {69817, 2, &rule84},
    {69819, 2, &rule2},
    {69821, 1, &rule16},
    {69822, 4, &rule2},
    {73728, 879, &rule45},
    {74752, 99, &rule116},
    {74864, 4, &rule2},
    {77824, 1071, &rule45},
    {92160, 569, &rule45},
    {110592, 2, &rule45},
    {118784, 246, &rule13},
    {119040, 39, &rule13},
    {119081, 60, &rule13},
    {119141, 2, &rule114},
    {119143, 3, &rule84},
    {119146, 3, &rule13},
    {119149, 6, &rule114},
    {119155, 8, &rule16},
    {119163, 8, &rule84},
    {119171, 2, &rule13},
    {119173, 7, &rule84},
    {119180, 30, &rule13},
    {119210, 4, &rule84},
    {119214, 48, &rule13},
    {119296, 66, &rule13},
    {119362, 3, &rule84},
    {119365, 1, &rule13},
    {119552, 87, &rule13},
    {119648, 18, &rule17},
    {119808, 26, &rule98},
    {119834, 26, &rule14},
    {119860, 26, &rule98},
    {119886, 7, &rule14},
    {119894, 18, &rule14},
    {119912, 26, &rule98},
    {119938, 26, &rule14},
    {119964, 1, &rule98},
    {119966, 2, &rule98},
    {119970, 1, &rule98},
    {119973, 2, &rule98},
    {119977, 4, &rule98},
    {119982, 8, &rule98},
    {119990, 4, &rule14},
    {119995, 1, &rule14},
    {119997, 7, &rule14},
    {120005, 11, &rule14},
    {120016, 26, &rule98},
    {120042, 26, &rule14},
    {120068, 2, &rule98},
    {120071, 4, &rule98},
    {120077, 8, &rule98},
    {120086, 7, &rule98},
    {120094, 26, &rule14},
    {120120, 2, &rule98},
    {120123, 4, &rule98},
    {120128, 5, &rule98},
    {120134, 1, &rule98},
    {120138, 7, &rule98},
    {120146, 26, &rule14},
    {120172, 26, &rule98},
    {120198, 26, &rule14},
    {120224, 26, &rule98},
    {120250, 26, &rule14},
    {120276, 26, &rule98},
    {120302, 26, &rule14},
    {120328, 26, &rule98},
    {120354, 26, &rule14},
    {120380, 26, &rule98},
    {120406, 26, &rule14},
    {120432, 26, &rule98},
    {120458, 28, &rule14},
    {120488, 25, &rule98},
    {120513, 1, &rule6},
    {120514, 25, &rule14},
    {120539, 1, &rule6},
    {120540, 6, &rule14},
    {120546, 25, &rule98},
    {120571, 1, &rule6},
    {120572, 25, &rule14},
    {120597, 1, &rule6},
    {120598, 6, &rule14},
    {120604, 25, &rule98},
    {120629, 1, &rule6},
    {120630, 25, &rule14},
    {120655, 1, &rule6},
    {120656, 6, &rule14},
    {120662, 25, &rule98},
    {120687, 1, &rule6},
    {120688, 25, &rule14},
    {120713, 1, &rule6},
    {120714, 6, &rule14},
    {120720, 25, &rule98},
    {120745, 1, &rule6},
    {120746, 25, &rule14},
    {120771, 1, &rule6},
    {120772, 6, &rule14},
    {120778, 1, &rule98},
    {120779, 1, &rule14},
    {120782, 50, &rule8},
    {126976, 44, &rule13},
    {127024, 100, &rule13},
    {127136, 15, &rule13},
    {127153, 14, &rule13},
    {127169, 15, &rule13},
    {127185, 15, &rule13},
    {127232, 11, &rule17},
    {127248, 31, &rule13},
    {127280, 58, &rule13},
    {127344, 43, &rule13},
    {127462, 29, &rule13},
    {127504, 43, &rule13},
    {127552, 9, &rule13},
    {127568, 2, &rule13},
    {127744, 33, &rule13},
    {127792, 6, &rule13},
    {127799, 70, &rule13},
    {127872, 20, &rule13},
    {127904, 37, &rule13},
    {127942, 5, &rule13},
    {127968, 17, &rule13},
    {128000, 63, &rule13},
    {128064, 1, &rule13},
    {128066, 182, &rule13},
    {128249, 4, &rule13},
    {128256, 62, &rule13},
    {128336, 24, &rule13},
    {128507, 5, &rule13},
    {128513, 16, &rule13},
    {128530, 3, &rule13},
    {128534, 1, &rule13},
    {128536, 1, &rule13},
    {128538, 1, &rule13},
    {128540, 3, &rule13},
    {128544, 6, &rule13},
    {128552, 4, &rule13},
    {128557, 1, &rule13},
    {128560, 4, &rule13},
    {128565, 12, &rule13},
    {128581, 11, &rule13},
    {128640, 70, &rule13},
    {128768, 116, &rule13},
    {131072, 42711, &rule45},
    {173824, 4149, &rule45},
    {177984, 222, &rule45},
    {194560, 542, &rule45},
    {917505, 1, &rule16},
    {917536, 96, &rule16},
    {917760, 240, &rule84},
    {983040, 65534, &rule164},
    {1048576, 65534, &rule164}
};
static const struct _charblock_ convchars[]={
    {65, 26, &rule9},
    {97, 26, &rule12},
    {181, 1, &rule18},
    {192, 23, &rule9},
    {216, 7, &rule9},
    {224, 23, &rule12},
    {248, 7, &rule12},
    {255, 1, &rule20},
    {256, 1, &rule21},
    {257, 1, &rule22},
    {258, 1, &rule21},
    {259, 1, &rule22},
    {260, 1, &rule21},
    {261, 1, &rule22},
    {262, 1, &rule21},
    {263, 1, &rule22},
    {264, 1, &rule21},
    {265, 1, &rule22},
    {266, 1, &rule21},
    {267, 1, &rule22},
    {268, 1, &rule21},
    {269, 1, &rule22},
    {270, 1, &rule21},
    {271, 1, &rule22},
    {272, 1, &rule21},
    {273, 1, &rule22},
    {274, 1, &rule21},
    {275, 1, &rule22},
    {276, 1, &rule21},
    {277, 1, &rule22},
    {278, 1, &rule21},
    {279, 1, &rule22},
    {280, 1, &rule21},
    {281, 1, &rule22},
    {282, 1, &rule21},
    {283, 1, &rule22},
    {284, 1, &rule21},
    {285, 1, &rule22},
    {286, 1, &rule21},
    {287, 1, &rule22},
    {288, 1, &rule21},
    {289, 1, &rule22},
    {290, 1, &rule21},
    {291, 1, &rule22},
    {292, 1, &rule21},
    {293, 1, &rule22},
    {294, 1, &rule21},
    {295, 1, &rule22},
    {296, 1, &rule21},
    {297, 1, &rule22},
    {298, 1, &rule21},
    {299, 1, &rule22},
    {300, 1, &rule21},
    {301, 1, &rule22},
    {302, 1, &rule21},
    {303, 1, &rule22},
    {304, 1, &rule23},
    {305, 1, &rule24},
    {306, 1, &rule21},
    {307, 1, &rule22},
    {308, 1, &rule21},
    {309, 1, &rule22},
    {310, 1, &rule21},
    {311, 1, &rule22},
    {313, 1, &rule21},
    {314, 1, &rule22},
    {315, 1, &rule21},
    {316, 1, &rule22},
    {317, 1, &rule21},
    {318, 1, &rule22},
    {319, 1, &rule21},
    {320, 1, &rule22},
    {321, 1, &rule21},
    {322, 1, &rule22},
    {323, 1, &rule21},
    {324, 1, &rule22},
    {325, 1, &rule21},
    {326, 1, &rule22},
    {327, 1, &rule21},
    {328, 1, &rule22},
    {330, 1, &rule21},
    {331, 1, &rule22},
    {332, 1, &rule21},
    {333, 1, &rule22},
    {334, 1, &rule21},
    {335, 1, &rule22},
    {336, 1, &rule21},
    {337, 1, &rule22},
    {338, 1, &rule21},
    {339, 1, &rule22},
    {340, 1, &rule21},
    {341, 1, &rule22},
    {342, 1, &rule21},
    {343, 1, &rule22},
    {344, 1, &rule21},
    {345, 1, &rule22},
    {346, 1, &rule21},
    {347, 1, &rule22},
    {348, 1, &rule21},
    {349, 1, &rule22},
    {350, 1, &rule21},
    {351, 1, &rule22},
    {352, 1, &rule21},
    {353, 1, &rule22},
    {354, 1, &rule21},
    {355, 1, &rule22},
    {356, 1, &rule21},
    {357, 1, &rule22},
    {358, 1, &rule21},
    {359, 1, &rule22},
    {360, 1, &rule21},
    {361, 1, &rule22},
    {362, 1, &rule21},
    {363, 1, &rule22},
    {364, 1, &rule21},
    {365, 1, &rule22},
    {366, 1, &rule21},
    {367, 1, &rule22},
    {368, 1, &rule21},
    {369, 1, &rule22},
    {370, 1, &rule21},
    {371, 1, &rule22},
    {372, 1, &rule21},
    {373, 1, &rule22},
    {374, 1, &rule21},
    {375, 1, &rule22},
    {376, 1, &rule25},
    {377, 1, &rule21},
    {378, 1, &rule22},
    {379, 1, &rule21},
    {380, 1, &rule22},
    {381, 1, &rule21},
    {382, 1, &rule22},
    {383, 1, &rule26},
    {384, 1, &rule27},
    {385, 1, &rule28},
    {386, 1, &rule21},
    {387, 1, &rule22},
    {388, 1, &rule21},
    {389, 1, &rule22},
    {390, 1, &rule29},
    {391, 1, &rule21},
    {392, 1, &rule22},
    {393, 2, &rule30},
    {395, 1, &rule21},
    {396, 1, &rule22},
    {398, 1, &rule31},
    {399, 1, &rule32},
    {400, 1, &rule33},
    {401, 1, &rule21},
    {402, 1, &rule22},
    {403, 1, &rule30},
    {404, 1, &rule34},
    {405, 1, &rule35},
    {406, 1, &rule36},
    {407, 1, &rule37},
    {408, 1, &rule21},
    {409, 1, &rule22},
    {410, 1, &rule38},
    {412, 1, &rule36},
    {413, 1, &rule39},
    {414, 1, &rule40},
    {415, 1, &rule41},
    {416, 1, &rule21},
    {417, 1, &rule22},
    {418, 1, &rule21},
    {419, 1, &rule22},
    {420, 1, &rule21},
    {421, 1, &rule22},
    {422, 1, &rule42},
    {423, 1, &rule21},
    {424, 1, &rule22},
    {425, 1, &rule42},
    {428, 1, &rule21},
    {429, 1, &rule22},
    {430, 1, &rule42},
    {431, 1, &rule21},
    {432, 1, &rule22},
    {433, 2, &rule43},
    {435, 1, &rule21},
    {436, 1, &rule22},
    {437, 1, &rule21},
    {438, 1, &rule22},
    {439, 1, &rule44},
    {440, 1, &rule21},
    {441, 1, &rule22},
    {444, 1, &rule21},
    {445, 1, &rule22},
    {447, 1, &rule46},
    {452, 1, &rule47},
    {453, 1, &rule48},
    {454, 1, &rule49},
    {455, 1, &rule47},
    {456, 1, &rule48},
    {457, 1, &rule49},
    {458, 1, &rule47},
    {459, 1, &rule48},
    {460, 1, &rule49},
    {461, 1, &rule21},
    {462, 1, &rule22},
    {463, 1, &rule21},
    {464, 1, &rule22},
    {465, 1, &rule21},
    {466, 1, &rule22},
    {467, 1, &rule21},
    {468, 1, &rule22},
    {469, 1, &rule21},
    {470, 1, &rule22},
    {471, 1, &rule21},
    {472, 1, &rule22},
    {473, 1, &rule21},
    {474, 1, &rule22},
    {475, 1, &rule21},
    {476, 1, &rule22},
    {477, 1, &rule50},
    {478, 1, &rule21},
    {479, 1, &rule22},
    {480, 1, &rule21},
    {481, 1, &rule22},
    {482, 1, &rule21},
    {483, 1, &rule22},
    {484, 1, &rule21},
    {485, 1, &rule22},
    {486, 1, &rule21},
    {487, 1, &rule22},
    {488, 1, &rule21},
    {489, 1, &rule22},
    {490, 1, &rule21},
    {491, 1, &rule22},
    {492, 1, &rule21},
    {493, 1, &rule22},
    {494, 1, &rule21},
    {495, 1, &rule22},
    {497, 1, &rule47},
    {498, 1, &rule48},
    {499, 1, &rule49},
    {500, 1, &rule21},
    {501, 1, &rule22},
    {502, 1, &rule51},
    {503, 1, &rule52},
    {504, 1, &rule21},
    {505, 1, &rule22},
    {506, 1, &rule21},
    {507, 1, &rule22},
    {508, 1, &rule21},
    {509, 1, &rule22},
    {510, 1, &rule21},
    {511, 1, &rule22},
    {512, 1, &rule21},
    {513, 1, &rule22},
    {514, 1, &rule21},
    {515, 1, &rule22},
    {516, 1, &rule21},
    {517, 1, &rule22},
    {518, 1, &rule21},
    {519, 1, &rule22},
    {520, 1, &rule21},
    {521, 1, &rule22},
    {522, 1, &rule21},
    {523, 1, &rule22},
    {524, 1, &rule21},
    {525, 1, &rule22},
    {526, 1, &rule21},
    {527, 1, &rule22},
    {528, 1, &rule21},
    {529, 1, &rule22},
    {530, 1, &rule21},
    {531, 1, &rule22},
    {532, 1, &rule21},
    {533, 1, &rule22},
    {534, 1, &rule21},
    {535, 1, &rule22},
    {536, 1, &rule21},
    {537, 1, &rule22},
    {538, 1, &rule21},
    {539, 1, &rule22},
    {540, 1, &rule21},
    {541, 1, &rule22},
    {542, 1, &rule21},
    {543, 1, &rule22},
    {544, 1, &rule53},
    {546, 1, &rule21},
    {547, 1, &rule22},
    {548, 1, &rule21},
    {549, 1, &rule22},
    {550, 1, &rule21},
    {551, 1, &rule22},
    {552, 1, &rule21},
    {553, 1, &rule22},
    {554, 1, &rule21},
    {555, 1, &rule22},
    {556, 1, &rule21},
    {557, 1, &rule22},
    {558, 1, &rule21},
    {559, 1, &rule22},
    {560, 1, &rule21},
    {561, 1, &rule22},
    {562, 1, &rule21},
    {563, 1, &rule22},
    {570, 1, &rule54},
    {571, 1, &rule21},
    {572, 1, &rule22},
    {573, 1, &rule55},
    {574, 1, &rule56},
    {575, 2, &rule57},
    {577, 1, &rule21},
    {578, 1, &rule22},
    {579, 1, &rule58},
    {580, 1, &rule59},
    {581, 1, &rule60},
    {582, 1, &rule21},
    {583, 1, &rule22},
    {584, 1, &rule21},
    {585, 1, &rule22},
    {586, 1, &rule21},
    {587, 1, &rule22},
    {588, 1, &rule21},
    {589, 1, &rule22},
    {590, 1, &rule21},
    {591, 1, &rule22},
    {592, 1, &rule61},
    {593, 1, &rule62},
    {594, 1, &rule63},
    {595, 1, &rule64},
    {596, 1, &rule65},
    {598, 2, &rule66},
    {601, 1, &rule67},
    {603, 1, &rule68},
    {608, 1, &rule66},
    {611, 1, &rule69},
    {613, 1, &rule70},
    {616, 1, &rule71},
    {617, 1, &rule72},
    {619, 1, &rule73},
    {623, 1, &rule72},
    {625, 1, &rule74},
    {626, 1, &rule75},
    {629, 1, &rule76},
    {637, 1, &rule77},
    {640, 1, &rule78},
    {643, 1, &rule78},
    {648, 1, &rule78},
    {649, 1, &rule79},
    {650, 2, &rule80},
    {652, 1, &rule81},
    {658, 1, &rule82},
    {837, 1, &rule85},
    {880, 1, &rule21},
    {881, 1, &rule22},
    {882, 1, &rule21},
    {883, 1, &rule22},
    {886, 1, &rule21},
    {887, 1, &rule22},
    {891, 3, &rule40},
    {902, 1, &rule86},
    {904, 3, &rule87},
    {908, 1, &rule88},
    {910, 2, &rule89},
    {913, 17, &rule9},
    {931, 9, &rule9},
    {940, 1, &rule90},
    {941, 3, &rule91},
    {945, 17, &rule12},
    {962, 1, &rule92},
    {963, 9, &rule12},
    {972, 1, &rule93},
    {973, 2, &rule94},
    {975, 1, &rule95},
    {976, 1, &rule96},
    {977, 1, &rule97},
    {981, 1, &rule99},
    {982, 1, &rule100},
    {983, 1, &rule101},
    {984, 1, &rule21},
    {985, 1, &rule22},
    {986, 1, &rule21},
    {987, 1, &rule22},
    {988, 1, &rule21},
    {989, 1, &rule22},
    {990, 1, &rule21},
    {991, 1, &rule22},
    {992, 1, &rule21},
    {993, 1, &rule22},
    {994, 1, &rule21},
    {995, 1, &rule22},
    {996, 1, &rule21},
    {997, 1, &rule22},
    {998, 1, &rule21},
    {999, 1, &rule22},
    {1000, 1, &rule21},
    {1001, 1, &rule22},
    {1002, 1, &rule21},
    {1003, 1, &rule22},
    {1004, 1, &rule21},
    {1005, 1, &rule22},
    {1006, 1, &rule21},
    {1007, 1, &rule22},
    {1008, 1, &rule102},
    {1009, 1, &rule103},
    {1010, 1, &rule104},
    {1012, 1, &rule105},
    {1013, 1, &rule106},
    {1015, 1, &rule21},
    {1016, 1, &rule22},
    {1017, 1, &rule107},
    {1018, 1, &rule21},
    {1019, 1, &rule22},
    {1021, 3, &rule53},
    {1024, 16, &rule108},
    {1040, 32, &rule9},
    {1072, 32, &rule12},
    {1104, 16, &rule103},
    {1120, 1, &rule21},
    {1121, 1, &rule22},
    {1122, 1, &rule21},
    {1123, 1, &rule22},
    {1124, 1, &rule21},
    {1125, 1, &rule22},
    {1126, 1, &rule21},
    {1127, 1, &rule22},
    {1128, 1, &rule21},
    {1129, 1, &rule22},
    {1130, 1, &rule21},
    {1131, 1, &rule22},
    {1132, 1, &rule21},
    {1133, 1, &rule22},
    {1134, 1, &rule21},
    {1135, 1, &rule22},
    {1136, 1, &rule21},
    {1137, 1, &rule22},
    {1138, 1, &rule21},
    {1139, 1, &rule22},
    {1140, 1, &rule21},
    {1141, 1, &rule22},
    {1142, 1, &rule21},
    {1143, 1, &rule22},
    {1144, 1, &rule21},
    {1145, 1, &rule22},
    {1146, 1, &rule21},
    {1147, 1, &rule22},
    {1148, 1, &rule21},
    {1149, 1, &rule22},
    {1150, 1, &rule21},
    {1151, 1, &rule22},
    {1152, 1, &rule21},
    {1153, 1, &rule22},
    {1162, 1, &rule21},
    {1163, 1, &rule22},
    {1164, 1, &rule21},
    {1165, 1, &rule22},
    {1166, 1, &rule21},
    {1167, 1, &rule22},
    {1168, 1, &rule21},
    {1169, 1, &rule22},
    {1170, 1, &rule21},
    {1171, 1, &rule22},
    {1172, 1, &rule21},
    {1173, 1, &rule22},
    {1174, 1, &rule21},
    {1175, 1, &rule22},
    {1176, 1, &rule21},
    {1177, 1, &rule22},
    {1178, 1, &rule21},
    {1179, 1, &rule22},
    {1180, 1, &rule21},
    {1181, 1, &rule22},
    {1182, 1, &rule21},
    {1183, 1, &rule22},
    {1184, 1, &rule21},
    {1185, 1, &rule22},
    {1186, 1, &rule21},
    {1187, 1, &rule22},
    {1188, 1, &rule21},
    {1189, 1, &rule22},
    {1190, 1, &rule21},
    {1191, 1, &rule22},
    {1192, 1, &rule21},
    {1193, 1, &rule22},
    {1194, 1, &rule21},
    {1195, 1, &rule22},
    {1196, 1, &rule21},
    {1197, 1, &rule22},
    {1198, 1, &rule21},
    {1199, 1, &rule22},
    {1200, 1, &rule21},
    {1201, 1, &rule22},
    {1202, 1, &rule21},
    {1203, 1, &rule22},
    {1204, 1, &rule21},
    {1205, 1, &rule22},
    {1206, 1, &rule21},
    {1207, 1, &rule22},
    {1208, 1, &rule21},
    {1209, 1, &rule22},
    {1210, 1, &rule21},
    {1211, 1, &rule22},
    {1212, 1, &rule21},
    {1213, 1, &rule22},
    {1214, 1, &rule21},
    {1215, 1, &rule22},
    {1216, 1, &rule110},
    {1217, 1, &rule21},
    {1218, 1, &rule22},
    {1219, 1, &rule21},
    {1220, 1, &rule22},
    {1221, 1, &rule21},
    {1222, 1, &rule22},
    {1223, 1, &rule21},
    {1224, 1, &rule22},
    {1225, 1, &rule21},
    {1226, 1, &rule22},
    {1227, 1, &rule21},
    {1228, 1, &rule22},
    {1229, 1, &rule21},
    {1230, 1, &rule22},
    {1231, 1, &rule111},
    {1232, 1, &rule21},
    {1233, 1, &rule22},
    {1234, 1, &rule21},
    {1235, 1, &rule22},
    {1236, 1, &rule21},
    {1237, 1, &rule22},
    {1238, 1, &rule21},
    {1239, 1, &rule22},
    {1240, 1, &rule21},
    {1241, 1, &rule22},
    {1242, 1, &rule21},
    {1243, 1, &rule22},
    {1244, 1, &rule21},
    {1245, 1, &rule22},
    {1246, 1, &rule21},
    {1247, 1, &rule22},
    {1248, 1, &rule21},
    {1249, 1, &rule22},
    {1250, 1, &rule21},
    {1251, 1, &rule22},
    {1252, 1, &rule21},
    {1253, 1, &rule22},
    {1254, 1, &rule21},
    {1255, 1, &rule22},
    {1256, 1, &rule21},
    {1257, 1, &rule22},
    {1258, 1, &rule21},
    {1259, 1, &rule22},
    {1260, 1, &rule21},
    {1261, 1, &rule22},
    {1262, 1, &rule21},
    {1263, 1, &rule22},
    {1264, 1, &rule21},
    {1265, 1, &rule22},
    {1266, 1, &rule21},
    {1267, 1, &rule22},
    {1268, 1, &rule21},
    {1269, 1, &rule22},
    {1270, 1, &rule21},
    {1271, 1, &rule22},
    {1272, 1, &rule21},
    {1273, 1, &rule22},
    {1274, 1, &rule21},
    {1275, 1, &rule22},
    {1276, 1, &rule21},
    {1277, 1, &rule22},
    {1278, 1, &rule21},
    {1279, 1, &rule22},
    {1280, 1, &rule21},
    {1281, 1, &rule22},
    {1282, 1, &rule21},
    {1283, 1, &rule22},
    {1284, 1, &rule21},
    {1285, 1, &rule22},
    {1286, 1, &rule21},
    {1287, 1, &rule22},
    {1288, 1, &rule21},
    {1289, 1, &rule22},
    {1290, 1, &rule21},
    {1291, 1, &rule22},
    {1292, 1, &rule21},
    {1293, 1, &rule22},
    {1294, 1, &rule21},
    {1295, 1, &rule22},
    {1296, 1, &rule21},
    {1297, 1, &rule22},
    {1298, 1, &rule21},
    {1299, 1, &rule22},
    {1300, 1, &rule21},
    {1301, 1, &rule22},
    {1302, 1, &rule21},
    {1303, 1, &rule22},
    {1304, 1, &rule21},
    {1305, 1, &rule22},
    {1306, 1, &rule21},
    {1307, 1, &rule22},
    {1308, 1, &rule21},
    {1309, 1, &rule22},
    {1310, 1, &rule21},
    {1311, 1, &rule22},
    {1312, 1, &rule21},
    {1313, 1, &rule22},
    {1314, 1, &rule21},
    {1315, 1, &rule22},
    {1316, 1, &rule21},
    {1317, 1, &rule22},
    {1318, 1, &rule21},
    {1319, 1, &rule22},
    {1329, 38, &rule112},
    {1377, 38, &rule113},
    {4256, 38, &rule115},
    {7545, 1, &rule117},
    {7549, 1, &rule118},
    {7680, 1, &rule21},
    {7681, 1, &rule22},
    {7682, 1, &rule21},
    {7683, 1, &rule22},
    {7684, 1, &rule21},
    {7685, 1, &rule22},
    {7686, 1, &rule21},
    {7687, 1, &rule22},
    {7688, 1, &rule21},
    {7689, 1, &rule22},
    {7690, 1, &rule21},
    {7691, 1, &rule22},
    {7692, 1, &rule21},
    {7693, 1, &rule22},
    {7694, 1, &rule21},
    {7695, 1, &rule22},
    {7696, 1, &rule21},
    {7697, 1, &rule22},
    {7698, 1, &rule21},
    {7699, 1, &rule22},
    {7700, 1, &rule21},
    {7701, 1, &rule22},
    {7702, 1, &rule21},
    {7703, 1, &rule22},
    {7704, 1, &rule21},
    {7705, 1, &rule22},
    {7706, 1, &rule21},
    {7707, 1, &rule22},
    {7708, 1, &rule21},
    {7709, 1, &rule22},
    {7710, 1, &rule21},
    {7711, 1, &rule22},
    {7712, 1, &rule21},
    {7713, 1, &rule22},
    {7714, 1, &rule21},
    {7715, 1, &rule22},
    {7716, 1, &rule21},
    {7717, 1, &rule22},
    {7718, 1, &rule21},
    {7719, 1, &rule22},
    {7720, 1, &rule21},
    {7721, 1, &rule22},
    {7722, 1, &rule21},
    {7723, 1, &rule22},
    {7724, 1, &rule21},
    {7725, 1, &rule22},
    {7726, 1, &rule21},
    {7727, 1, &rule22},
    {7728, 1, &rule21},
    {7729, 1, &rule22},
    {7730, 1, &rule21},
    {7731, 1, &rule22},
    {7732, 1, &rule21},
    {7733, 1, &rule22},
    {7734, 1, &rule21},
    {7735, 1, &rule22},
    {7736, 1, &rule21},
    {7737, 1, &rule22},
    {7738, 1, &rule21},
    {7739, 1, &rule22},
    {7740, 1, &rule21},
    {7741, 1, &rule22},
    {7742, 1, &rule21},
    {7743, 1, &rule22},
    {7744, 1, &rule21},
    {7745, 1, &rule22},
    {7746, 1, &rule21},
    {7747, 1, &rule22},
    {7748, 1, &rule21},
    {7749, 1, &rule22},
    {7750, 1, &rule21},
    {7751, 1, &rule22},
    {7752, 1, &rule21},
    {7753, 1, &rule22},
    {7754, 1, &rule21},
    {7755, 1, &rule22},
    {7756, 1, &rule21},
    {7757, 1, &rule22},
    {7758, 1, &rule21},
    {7759, 1, &rule22},
    {7760, 1, &rule21},
    {7761, 1, &rule22},
    {7762, 1, &rule21},
    {7763, 1, &rule22},
    {7764, 1, &rule21},
    {7765, 1, &rule22},
    {7766, 1, &rule21},
    {7767, 1, &rule22},
    {7768, 1, &rule21},
    {7769, 1, &rule22},
    {7770, 1, &rule21},
    {7771, 1, &rule22},
    {7772, 1, &rule21},
    {7773, 1, &rule22},
    {7774, 1, &rule21},
    {7775, 1, &rule22},
    {7776, 1, &rule21},
    {7777, 1, &rule22},
    {7778, 1, &rule21},
    {7779, 1, &rule22},
    {7780, 1, &rule21},
    {7781, 1, &rule22},
    {7782, 1, &rule21},
    {7783, 1, &rule22},
    {7784, 1, &rule21},
    {7785, 1, &rule22},
    {7786, 1, &rule21},
    {7787, 1, &rule22},
    {7788, 1, &rule21},
    {7789, 1, &rule22},
    {7790, 1, &rule21},
    {7791, 1, &rule22},
    {7792, 1, &rule21},
    {7793, 1, &rule22},
    {7794, 1, &rule21},
    {7795, 1, &rule22},
    {7796, 1, &rule21},
    {7797, 1, &rule22},
    {7798, 1, &rule21},
    {7799, 1, &rule22},
    {7800, 1, &rule21},
    {7801, 1, &rule22},
    {7802, 1, &rule21},
    {7803, 1, &rule22},
    {7804, 1, &rule21},
    {7805, 1, &rule22},
    {7806, 1, &rule21},
    {7807, 1, &rule22},
    {7808, 1, &rule21},
    {7809, 1, &rule22},
    {7810, 1, &rule21},
    {7811, 1, &rule22},
    {7812, 1, &rule21},
    {7813, 1, &rule22},
    {7814, 1, &rule21},
    {7815, 1, &rule22},
    {7816, 1, &rule21},
    {7817, 1, &rule22},
    {7818, 1, &rule21},
    {7819, 1, &rule22},
    {7820, 1, &rule21},
    {7821, 1, &rule22},
    {7822, 1, &rule21},
    {7823, 1, &rule22},
    {7824, 1, &rule21},
    {7825, 1, &rule22},
    {7826, 1, &rule21},
    {7827, 1, &rule22},
    {7828, 1, &rule21},
    {7829, 1, &rule22},
    {7835, 1, &rule119},
    {7838, 1, &rule120},
    {7840, 1, &rule21},
    {7841, 1, &rule22},
    {7842, 1, &rule21},
    {7843, 1, &rule22},
    {7844, 1, &rule21},
    {7845, 1, &rule22},
    {7846, 1, &rule21},
    {7847, 1, &rule22},
    {7848, 1, &rule21},
    {7849, 1, &rule22},
    {7850, 1, &rule21},
    {7851, 1, &rule22},
    {7852, 1, &rule21},
    {7853, 1, &rule22},
    {7854, 1, &rule21},
    {7855, 1, &rule22},
    {7856, 1, &rule21},
    {7857, 1, &rule22},
    {7858, 1, &rule21},
    {7859, 1, &rule22},
    {7860, 1, &rule21},
    {7861, 1, &rule22},
    {7862, 1, &rule21},
    {7863, 1, &rule22},
    {7864, 1, &rule21},
    {7865, 1, &rule22},
    {7866, 1, &rule21},
    {7867, 1, &rule22},
    {7868, 1, &rule21},
    {7869, 1, &rule22},
    {7870, 1, &rule21},
    {7871, 1, &rule22},
    {7872, 1, &rule21},
    {7873, 1, &rule22},
    {7874, 1, &rule21},
    {7875, 1, &rule22},
    {7876, 1, &rule21},
    {7877, 1, &rule22},
    {7878, 1, &rule21},
    {7879, 1, &rule22},
    {7880, 1, &rule21},
    {7881, 1, &rule22},
    {7882, 1, &rule21},
    {7883, 1, &rule22},
    {7884, 1, &rule21},
    {7885, 1, &rule22},
    {7886, 1, &rule21},
    {7887, 1, &rule22},
    {7888, 1, &rule21},
    {7889, 1, &rule22},
    {7890, 1, &rule21},
    {7891, 1, &rule22},
    {7892, 1, &rule21},
    {7893, 1, &rule22},
    {7894, 1, &rule21},
    {7895, 1, &rule22},
    {7896, 1, &rule21},
    {7897, 1, &rule22},
    {7898, 1, &rule21},
    {7899, 1, &rule22},
    {7900, 1, &rule21},
    {7901, 1, &rule22},
    {7902, 1, &rule21},
    {7903, 1, &rule22},
    {7904, 1, &rule21},
    {7905, 1, &rule22},
    {7906, 1, &rule21},
    {7907, 1, &rule22},
    {7908, 1, &rule21},
    {7909, 1, &rule22},
    {7910, 1, &rule21},
    {7911, 1, &rule22},
    {7912, 1, &rule21},
    {7913, 1, &rule22},
    {7914, 1, &rule21},
    {7915, 1, &rule22},
    {7916, 1, &rule21},
    {7917, 1, &rule22},
    {7918, 1, &rule21},
    {7919, 1, &rule22},
    {7920, 1, &rule21},
    {7921, 1, &rule22},
    {7922, 1, &rule21},
    {7923, 1, &rule22},
    {7924, 1, &rule21},
    {7925, 1, &rule22},
    {7926, 1, &rule21},
    {7927, 1, &rule22},
    {7928, 1, &rule21},
    {7929, 1, &rule22},
    {7930, 1, &rule21},
    {7931, 1, &rule22},
    {7932, 1, &rule21},
    {7933, 1, &rule22},
    {7934, 1, &rule21},
    {7935, 1, &rule22},
    {7936, 8, &rule121},
    {7944, 8, &rule122},
    {7952, 6, &rule121},
    {7960, 6, &rule122},
    {7968, 8, &rule121},
    {7976, 8, &rule122},
    {7984, 8, &rule121},
    {7992, 8, &rule122},
    {8000, 6, &rule121},
    {8008, 6, &rule122},
    {8017, 1, &rule121},
    {8019, 1, &rule121},
    {8021, 1, &rule121},
    {8023, 1, &rule121},
    {8025, 1, &rule122},
    {8027, 1, &rule122},
    {8029, 1, &rule122},
    {8031, 1, &rule122},
    {8032, 8, &rule121},
    {8040, 8, &rule122},
    {8048, 2, &rule123},
    {8050, 4, &rule124},
    {8054, 2, &rule125},
    {8056, 2, &rule126},
    {8058, 2, &rule127},
    {8060, 2, &rule128},
    {8064, 8, &rule121},
    {8072, 8, &rule129},
    {8080, 8, &rule121},
    {8088, 8, &rule129},
    {8096, 8, &rule121},
    {8104, 8, &rule129},
    {8112, 2, &rule121},
    {8115, 1, &rule130},
    {8120, 2, &rule122},
    {8122, 2, &rule131},
    {8124, 1, &rule132},
    {8126, 1, &rule133},
    {8131, 1, &rule130},
    {8136, 4, &rule134},
    {8140, 1, &rule132},
    {8144, 2, &rule121},
    {8152, 2, &rule122},
    {8154, 2, &rule135},
    {8160, 2, &rule121},
    {8165, 1, &rule104},
    {8168, 2, &rule122},
    {8170, 2, &rule136},
    {8172, 1, &rule107},
    {8179, 1, &rule130},
    {8184, 2, &rule137},
    {8186, 2, &rule138},
    {8188, 1, &rule132},
    {8486, 1, &rule141},
    {8490, 1, &rule142},
    {8491, 1, &rule143},
    {8498, 1, &rule144},
    {8526, 1, &rule145},
    {8544, 16, &rule146},
    {8560, 16, &rule147},
    {8579, 1, &rule21},
    {8580, 1, &rule22},
    {9398, 26, &rule148},
    {9424, 26, &rule149},
    {11264, 47, &rule112},
    {11312, 47, &rule113},
    {11360, 1, &rule21},
    {11361, 1, &rule22},
    {11362, 1, &rule150},
    {11363, 1, &rule151},
    {11364, 1, &rule152},
    {11365, 1, &rule153},
    {11366, 1, &rule154},
    {11367, 1, &rule21},
    {11368, 1, &rule22},
    {11369, 1, &rule21},
    {11370, 1, &rule22},
    {11371, 1, &rule21},
    {11372, 1, &rule22},
    {11373, 1, &rule155},
    {11374, 1, &rule156},
    {11375, 1, &rule157},
    {11376, 1, &rule158},
    {11378, 1, &rule21},
    {11379, 1, &rule22},
    {11381, 1, &rule21},
    {11382, 1, &rule22},
    {11390, 2, &rule159},
    {11392, 1, &rule21},
    {11393, 1, &rule22},
    {11394, 1, &rule21},
    {11395, 1, &rule22},
    {11396, 1, &rule21},
    {11397, 1, &rule22},
    {11398, 1, &rule21},
    {11399, 1, &rule22},
    {11400, 1, &rule21},
    {11401, 1, &rule22},
    {11402, 1, &rule21},
    {11403, 1, &rule22},
    {11404, 1, &rule21},
    {11405, 1, &rule22},
    {11406, 1, &rule21},
    {11407, 1, &rule22},
    {11408, 1, &rule21},
    {11409, 1, &rule22},
    {11410, 1, &rule21},
    {11411, 1, &rule22},
    {11412, 1, &rule21},
    {11413, 1, &rule22},
    {11414, 1, &rule21},
    {11415, 1, &rule22},
    {11416, 1, &rule21},
    {11417, 1, &rule22},
    {11418, 1, &rule21},
    {11419, 1, &rule22},
    {11420, 1, &rule21},
    {11421, 1, &rule22},
    {11422, 1, &rule21},
    {11423, 1, &rule22},
    {11424, 1, &rule21},
    {11425, 1, &rule22},
    {11426, 1, &rule21},
    {11427, 1, &rule22},
    {11428, 1, &rule21},
    {11429, 1, &rule22},
    {11430, 1, &rule21},
    {11431, 1, &rule22},
    {11432, 1, &rule21},
    {11433, 1, &rule22},
    {11434, 1, &rule21},
    {11435, 1, &rule22},
    {11436, 1, &rule21},
    {11437, 1, &rule22},
    {11438, 1, &rule21},
    {11439, 1, &rule22},
    {11440, 1, &rule21},
    {11441, 1, &rule22},
    {11442, 1, &rule21},
    {11443, 1, &rule22},
    {11444, 1, &rule21},
    {11445, 1, &rule22},
    {11446, 1, &rule21},
    {11447, 1, &rule22},
    {11448, 1, &rule21},
    {11449, 1, &rule22},
    {11450, 1, &rule21},
    {11451, 1, &rule22},
    {11452, 1, &rule21},
    {11453, 1, &rule22},
    {11454, 1, &rule21},
    {11455, 1, &rule22},
    {11456, 1, &rule21},
    {11457, 1, &rule22},
    {11458, 1, &rule21},
    {11459, 1, &rule22},
    {11460, 1, &rule21},
    {11461, 1, &rule22},
    {11462, 1, &rule21},
    {11463, 1, &rule22},
    {11464, 1, &rule21},
    {11465, 1, &rule22},
    {11466, 1, &rule21},
    {11467, 1, &rule22},
    {11468, 1, &rule21},
    {11469, 1, &rule22},
    {11470, 1, &rule21},
    {11471, 1, &rule22},
    {11472, 1, &rule21},
    {11473, 1, &rule22},
    {11474, 1, &rule21},
    {11475, 1, &rule22},
    {11476, 1, &rule21},
    {11477, 1, &rule22},
    {11478, 1, &rule21},
    {11479, 1, &rule22},
    {11480, 1, &rule21},
    {11481, 1, &rule22},
    {11482, 1, &rule21},
    {11483, 1, &rule22},
    {11484, 1, &rule21},
    {11485, 1, &rule22},
    {11486, 1, &rule21},
    {11487, 1, &rule22},
    {11488, 1, &rule21},
    {11489, 1, &rule22},
    {11490, 1, &rule21},
    {11491, 1, &rule22},
    {11499, 1, &rule21},
    {11500, 1, &rule22},
    {11501, 1, &rule21},
    {11502, 1, &rule22},
    {11520, 38, &rule160},
    {42560, 1, &rule21},
    {42561, 1, &rule22},
    {42562, 1, &rule21},
    {42563, 1, &rule22},
    {42564, 1, &rule21},
    {42565, 1, &rule22},
    {42566, 1, &rule21},
    {42567, 1, &rule22},
    {42568, 1, &rule21},
    {42569, 1, &rule22},
    {42570, 1, &rule21},
    {42571, 1, &rule22},
    {42572, 1, &rule21},
    {42573, 1, &rule22},
    {42574, 1, &rule21},
    {42575, 1, &rule22},
    {42576, 1, &rule21},
    {42577, 1, &rule22},
    {42578, 1, &rule21},
    {42579, 1, &rule22},
    {42580, 1, &rule21},
    {42581, 1, &rule22},
    {42582, 1, &rule21},
    {42583, 1, &rule22},
    {42584, 1, &rule21},
    {42585, 1, &rule22},
    {42586, 1, &rule21},
    {42587, 1, &rule22},
    {42588, 1, &rule21},
    {42589, 1, &rule22},
    {42590, 1, &rule21},
    {42591, 1, &rule22},
    {42592, 1, &rule21},
    {42593, 1, &rule22},
    {42594, 1, &rule21},
    {42595, 1, &rule22},
    {42596, 1, &rule21},
    {42597, 1, &rule22},
    {42598, 1, &rule21},
    {42599, 1, &rule22},
    {42600, 1, &rule21},
    {42601, 1, &rule22},
    {42602, 1, &rule21},
    {42603, 1, &rule22},
    {42604, 1, &rule21},
    {42605, 1, &rule22},
    {42624, 1, &rule21},
    {42625, 1, &rule22},
    {42626, 1, &rule21},
    {42627, 1, &rule22},
    {42628, 1, &rule21},
    {42629, 1, &rule22},
    {42630, 1, &rule21},
    {42631, 1, &rule22},
    {42632, 1, &rule21},
    {42633, 1, &rule22},
    {42634, 1, &rule21},
    {42635, 1, &rule22},
    {42636, 1, &rule21},
    {42637, 1, &rule22},
    {42638, 1, &rule21},
    {42639, 1, &rule22},
    {42640, 1, &rule21},
    {42641, 1, &rule22},
    {42642, 1, &rule21},
    {42643, 1, &rule22},
    {42644, 1, &rule21},
    {42645, 1, &rule22},
    {42646, 1, &rule21},
    {42647, 1, &rule22},
    {42786, 1, &rule21},
    {42787, 1, &rule22},
    {42788, 1, &rule21},
    {42789, 1, &rule22},
    {42790, 1, &rule21},
    {42791, 1, &rule22},
    {42792, 1, &rule21},
    {42793, 1, &rule22},
    {42794, 1, &rule21},
    {42795, 1, &rule22},
    {42796, 1, &rule21},
    {42797, 1, &rule22},
    {42798, 1, &rule21},
    {42799, 1, &rule22},
    {42802, 1, &rule21},
    {42803, 1, &rule22},
    {42804, 1, &rule21},
    {42805, 1, &rule22},
    {42806, 1, &rule21},
    {42807, 1, &rule22},
    {42808, 1, &rule21},
    {42809, 1, &rule22},
    {42810, 1, &rule21},
    {42811, 1, &rule22},
    {42812, 1, &rule21},
    {42813, 1, &rule22},
    {42814, 1, &rule21},
    {42815, 1, &rule22},
    {42816, 1, &rule21},
    {42817, 1, &rule22},
    {42818, 1, &rule21},
    {42819, 1, &rule22},
    {42820, 1, &rule21},
    {42821, 1, &rule22},
    {42822, 1, &rule21},
    {42823, 1, &rule22},
    {42824, 1, &rule21},
    {42825, 1, &rule22},
    {42826, 1, &rule21},
    {42827, 1, &rule22},
    {42828, 1, &rule21},
    {42829, 1, &rule22},
    {42830, 1, &rule21},
    {42831, 1, &rule22},
    {42832, 1, &rule21},
    {42833, 1, &rule22},
    {42834, 1, &rule21},
    {42835, 1, &rule22},
    {42836, 1, &rule21},
    {42837, 1, &rule22},
    {42838, 1, &rule21},
    {42839, 1, &rule22},
    {42840, 1, &rule21},
    {42841, 1, &rule22},
    {42842, 1, &rule21},
    {42843, 1, &rule22},
    {42844, 1, &rule21},
    {42845, 1, &rule22},
    {42846, 1, &rule21},
    {42847, 1, &rule22},
    {42848, 1, &rule21},
    {42849, 1, &rule22},
    {42850, 1, &rule21},
    {42851, 1, &rule22},
    {42852, 1, &rule21},
    {42853, 1, &rule22},
    {42854, 1, &rule21},
    {42855, 1, &rule22},
    {42856, 1, &rule21},
    {42857, 1, &rule22},
    {42858, 1, &rule21},
    {42859, 1, &rule22},
    {42860, 1, &rule21},
    {42861, 1, &rule22},
    {42862, 1, &rule21},
    {42863, 1, &rule22},
    {42873, 1, &rule21},
    {42874, 1, &rule22},
    {42875, 1, &rule21},
    {42876, 1, &rule22},
    {42877, 1, &rule161},
    {42878, 1, &rule21},
    {42879, 1, &rule22},
    {42880, 1, &rule21},
    {42881, 1, &rule22},
    {42882, 1, &rule21},
    {42883, 1, &rule22},
    {42884, 1, &rule21},
    {42885, 1, &rule22},
    {42886, 1, &rule21},
    {42887, 1, &rule22},
    {42891, 1, &rule21},
    {42892, 1, &rule22},
    {42893, 1, &rule162},
    {42896, 1, &rule21},
    {42897, 1, &rule22},
    {42912, 1, &rule21},
    {42913, 1, &rule22},
    {42914, 1, &rule21},
    {42915, 1, &rule22},
    {42916, 1, &rule21},
    {42917, 1, &rule22},
    {42918, 1, &rule21},
    {42919, 1, &rule22},
    {42920, 1, &rule21},
    {42921, 1, &rule22},
    {65313, 26, &rule9},
    {65345, 26, &rule12},
    {66560, 40, &rule165},
    {66600, 40, &rule166}
};
static const struct _charblock_ spacechars[]={
    {32, 1, &rule1},
    {160, 1, &rule1},
    {5760, 1, &rule1},
    {6158, 1, &rule1},
    {8192, 11, &rule1},
    {8239, 1, &rule1},
    {8287, 1, &rule1},
    {12288, 1, &rule1}
};

/*
    Obtain the reference to character rule by doing
    binary search over the specified array of blocks.
    To make checkattr shorter, the address of
    nullrule is returned if the search fails:
    this rule defines no category and no conversion
    distances. The compare function returns 0 when
    key->start is within the block. Otherwise
    result of comparison of key->start and start of the
    current block is returned as usual.
*/

static const struct _convrule_ nullrule={0,NUMCAT_CN,0,0,0,0};

int blkcmp(const void *vk,const void *vb)
{
    const struct _charblock_ *key,*cur;
    key=static_cast<const _charblock_*>(vk);
    cur=static_cast<const _charblock_*>(vb);
    if((key->start>=cur->start)&&(key->start<(cur->start+cur->length)))
    {
        return 0;
    }
    if(key->start>cur->start) return 1;
    return -1;
}

static const struct _convrule_ *getrule(
    const struct _charblock_ *blocks,
    int numblocks,
    int unichar)
{
    struct _charblock_ key={unichar,1,static_cast<const _convrule_*>((void *)0)};
    struct _charblock_ *cb=static_cast<_charblock_*>(bsearch(static_cast<_charblock_*>(&key),blocks,numblocks,sizeof(key),blkcmp));
    if(cb==(void *)0) return &nullrule;
    return cb->rule;
}



/*
    Check whether a character (internal code) has certain attributes.
    Attributes (category flags) may be ORed. The function ANDs
    character category flags and the mask and returns the result.
    If the character belongs to one of the categories requested,
    the result will be nonzero.
*/

inline static int checkattr(int c,unsigned int catmask)
{
    return (catmask & (getrule(allchars,(c<256)?NUM_LAT1BLOCKS:NUM_BLOCKS,c)->category));
}

inline static int checkattr_s(int c,unsigned int catmask)
{
        return (catmask & (getrule(spacechars,NUM_SPACEBLOCKS,c)->category));
}

/*
    Define predicate functions for some combinations of categories.
*/

#define unipred(p,m) \
int p(int c) \
{ \
    return checkattr(c,m); \
}

#define unipred_s(p,m) \
int p(int c) \
{ \
        return checkattr_s(c,m); \
}

/*
    Make these rules as close to Hugs as possible.
*/

unipred(u_iswcntrl,GENCAT_CC)
unipred(u_iswprint, (GENCAT_MC | GENCAT_NO | GENCAT_SK | GENCAT_ME | GENCAT_ND |   GENCAT_PO | GENCAT_LT | GENCAT_PC | GENCAT_SM | GENCAT_ZS |   GENCAT_LU | GENCAT_PD | GENCAT_SO | GENCAT_PE | GENCAT_PF |   GENCAT_PS | GENCAT_SC | GENCAT_LL | GENCAT_LM | GENCAT_PI |   GENCAT_NL | GENCAT_MN | GENCAT_LO))
unipred_s(u_iswspace,GENCAT_ZS)
unipred(u_iswupper,(GENCAT_LU|GENCAT_LT))
unipred(u_iswlower,GENCAT_LL)
unipred(u_iswalpha,(GENCAT_LL|GENCAT_LU|GENCAT_LT|GENCAT_LM|GENCAT_LO))
unipred(u_iswdigit,GENCAT_ND)

unipred(u_iswalnum,(GENCAT_LT|GENCAT_LU|GENCAT_LL|GENCAT_LM|GENCAT_LO|
            GENCAT_MC|GENCAT_ME|GENCAT_MN|
            GENCAT_NO|GENCAT_ND|GENCAT_NL))

#define caseconv(p,to) \
int p(int c) \
{ \
    const struct _convrule_ *rule=getrule(convchars,NUM_CONVBLOCKS,c);\
    if(rule==&nullrule) return c;\
    return c+rule->to;\
}

caseconv(u_towupper,updist)
caseconv(u_towlower,lowdist)
caseconv(u_towtitle,titledist)

int u_gencat(int c)
{
    return getrule(allchars,NUM_BLOCKS,c)->catnumber;
}






// from https://github.com/ghc/ghc/blob/master/compiler/parser/Lexer.x#L1682
unipred(u_IsHaskellSymbol,
   (    GENCAT_PC  // ConnectorPunctuation
      | GENCAT_PD  // DashPunctuation
      | GENCAT_PO  // OtherPunctuation
      | GENCAT_SM  // MathSymbol
      | GENCAT_SC  // CurrencySymbol
      | GENCAT_SK  // ModifierSymbol
      | GENCAT_SO  // OtherSymbol
   )
   )







#endif
