/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_GRAM_H_INCLUDED
# define YY_YY_GRAM_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    COLON = 258,                   /* COLON  */
    SEMICOLON = 259,               /* SEMICOLON  */
    LPAREN = 260,                  /* LPAREN  */
    RPAREN = 261,                  /* RPAREN  */
    LANGLE = 262,                  /* LANGLE  */
    RANGLE = 263,                  /* RANGLE  */
    LCURLY = 264,                  /* LCURLY  */
    RCURLY = 265,                  /* RCURLY  */
    LSQUARE = 266,                 /* LSQUARE  */
    RSQUARE = 267,                 /* RSQUARE  */
    COMMA = 268,                   /* COMMA  */
    EQ = 269,                      /* EQ  */
    ABS = 270,                     /* ABS  */
    DOT = 271,                     /* DOT  */
    PLUS = 272,                    /* PLUS  */
    MINUS = 273,                   /* MINUS  */
    MULTIPLY = 274,                /* MULTIPLY  */
    DIVIDE = 275,                  /* DIVIDE  */
    TYPE_UD = 276,                 /* TYPE_UD  */
    TYPE_D = 277,                  /* TYPE_D  */
    TYPE_UW = 278,                 /* TYPE_UW  */
    TYPE_W = 279,                  /* TYPE_W  */
    TYPE_UB = 280,                 /* TYPE_UB  */
    TYPE_B = 281,                  /* TYPE_B  */
    TYPE_VF = 282,                 /* TYPE_VF  */
    TYPE_HF = 283,                 /* TYPE_HF  */
    TYPE_V = 284,                  /* TYPE_V  */
    TYPE_F = 285,                  /* TYPE_F  */
    ALIGN1 = 286,                  /* ALIGN1  */
    ALIGN16 = 287,                 /* ALIGN16  */
    SECHALF = 288,                 /* SECHALF  */
    COMPR = 289,                   /* COMPR  */
    SWITCH = 290,                  /* SWITCH  */
    ATOMIC = 291,                  /* ATOMIC  */
    NODDCHK = 292,                 /* NODDCHK  */
    NODDCLR = 293,                 /* NODDCLR  */
    MASK_DISABLE = 294,            /* MASK_DISABLE  */
    BREAKPOINT = 295,              /* BREAKPOINT  */
    ACCWRCTRL = 296,               /* ACCWRCTRL  */
    EOT = 297,                     /* EOT  */
    SEQ = 298,                     /* SEQ  */
    ANY2H = 299,                   /* ANY2H  */
    ALL2H = 300,                   /* ALL2H  */
    ANY4H = 301,                   /* ANY4H  */
    ALL4H = 302,                   /* ALL4H  */
    ANY8H = 303,                   /* ANY8H  */
    ALL8H = 304,                   /* ALL8H  */
    ANY16H = 305,                  /* ANY16H  */
    ALL16H = 306,                  /* ALL16H  */
    ANYV = 307,                    /* ANYV  */
    ALLV = 308,                    /* ALLV  */
    ZERO = 309,                    /* ZERO  */
    EQUAL = 310,                   /* EQUAL  */
    NOT_ZERO = 311,                /* NOT_ZERO  */
    NOT_EQUAL = 312,               /* NOT_EQUAL  */
    GREATER = 313,                 /* GREATER  */
    GREATER_EQUAL = 314,           /* GREATER_EQUAL  */
    LESS = 315,                    /* LESS  */
    LESS_EQUAL = 316,              /* LESS_EQUAL  */
    ROUND_INCREMENT = 317,         /* ROUND_INCREMENT  */
    OVERFLOW = 318,                /* OVERFLOW  */
    UNORDERED = 319,               /* UNORDERED  */
    GENREG = 320,                  /* GENREG  */
    MSGREG = 321,                  /* MSGREG  */
    ADDRESSREG = 322,              /* ADDRESSREG  */
    ACCREG = 323,                  /* ACCREG  */
    FLAGREG = 324,                 /* FLAGREG  */
    MASKREG = 325,                 /* MASKREG  */
    AMASK = 326,                   /* AMASK  */
    IMASK = 327,                   /* IMASK  */
    LMASK = 328,                   /* LMASK  */
    CMASK = 329,                   /* CMASK  */
    MASKSTACKREG = 330,            /* MASKSTACKREG  */
    LMS = 331,                     /* LMS  */
    IMS = 332,                     /* IMS  */
    MASKSTACKDEPTHREG = 333,       /* MASKSTACKDEPTHREG  */
    IMSD = 334,                    /* IMSD  */
    LMSD = 335,                    /* LMSD  */
    NOTIFYREG = 336,               /* NOTIFYREG  */
    STATEREG = 337,                /* STATEREG  */
    CONTROLREG = 338,              /* CONTROLREG  */
    IPREG = 339,                   /* IPREG  */
    GENREGFILE = 340,              /* GENREGFILE  */
    MSGREGFILE = 341,              /* MSGREGFILE  */
    MOV = 342,                     /* MOV  */
    FRC = 343,                     /* FRC  */
    RNDU = 344,                    /* RNDU  */
    RNDD = 345,                    /* RNDD  */
    RNDE = 346,                    /* RNDE  */
    RNDZ = 347,                    /* RNDZ  */
    NOT = 348,                     /* NOT  */
    LZD = 349,                     /* LZD  */
    MUL = 350,                     /* MUL  */
    MAC = 351,                     /* MAC  */
    MACH = 352,                    /* MACH  */
    LINE = 353,                    /* LINE  */
    SAD2 = 354,                    /* SAD2  */
    SADA2 = 355,                   /* SADA2  */
    DP4 = 356,                     /* DP4  */
    DPH = 357,                     /* DPH  */
    DP3 = 358,                     /* DP3  */
    DP2 = 359,                     /* DP2  */
    AVG = 360,                     /* AVG  */
    ADD = 361,                     /* ADD  */
    SEL = 362,                     /* SEL  */
    AND = 363,                     /* AND  */
    OR = 364,                      /* OR  */
    XOR = 365,                     /* XOR  */
    SHR = 366,                     /* SHR  */
    SHL = 367,                     /* SHL  */
    ASR = 368,                     /* ASR  */
    CMP = 369,                     /* CMP  */
    CMPN = 370,                    /* CMPN  */
    PLN = 371,                     /* PLN  */
    ADDC = 372,                    /* ADDC  */
    BFI1 = 373,                    /* BFI1  */
    BFREV = 374,                   /* BFREV  */
    CBIT = 375,                    /* CBIT  */
    F16TO32 = 376,                 /* F16TO32  */
    F32TO16 = 377,                 /* F32TO16  */
    FBH = 378,                     /* FBH  */
    FBL = 379,                     /* FBL  */
    SEND = 380,                    /* SEND  */
    SENDC = 381,                   /* SENDC  */
    NOP = 382,                     /* NOP  */
    JMPI = 383,                    /* JMPI  */
    IF = 384,                      /* IF  */
    IFF = 385,                     /* IFF  */
    WHILE = 386,                   /* WHILE  */
    ELSE = 387,                    /* ELSE  */
    BREAK = 388,                   /* BREAK  */
    CONT = 389,                    /* CONT  */
    HALT = 390,                    /* HALT  */
    MSAVE = 391,                   /* MSAVE  */
    PUSH = 392,                    /* PUSH  */
    MREST = 393,                   /* MREST  */
    POP = 394,                     /* POP  */
    WAIT = 395,                    /* WAIT  */
    DO = 396,                      /* DO  */
    ENDIF = 397,                   /* ENDIF  */
    ILLEGAL = 398,                 /* ILLEGAL  */
    MATH_INST = 399,               /* MATH_INST  */
    MAD = 400,                     /* MAD  */
    LRP = 401,                     /* LRP  */
    BFE = 402,                     /* BFE  */
    BFI2 = 403,                    /* BFI2  */
    SUBB = 404,                    /* SUBB  */
    CALL = 405,                    /* CALL  */
    RET = 406,                     /* RET  */
    BRD = 407,                     /* BRD  */
    BRC = 408,                     /* BRC  */
    NULL_TOKEN = 409,              /* NULL_TOKEN  */
    MATH = 410,                    /* MATH  */
    SAMPLER = 411,                 /* SAMPLER  */
    GATEWAY = 412,                 /* GATEWAY  */
    READ = 413,                    /* READ  */
    WRITE = 414,                   /* WRITE  */
    URB = 415,                     /* URB  */
    THREAD_SPAWNER = 416,          /* THREAD_SPAWNER  */
    VME = 417,                     /* VME  */
    DATA_PORT = 418,               /* DATA_PORT  */
    CRE = 419,                     /* CRE  */
    MSGLEN = 420,                  /* MSGLEN  */
    RETURNLEN = 421,               /* RETURNLEN  */
    ALLOCATE = 422,                /* ALLOCATE  */
    USED = 423,                    /* USED  */
    COMPLETE = 424,                /* COMPLETE  */
    TRANSPOSE = 425,               /* TRANSPOSE  */
    INTERLEAVE = 426,              /* INTERLEAVE  */
    SATURATE = 427,                /* SATURATE  */
    INTEGER = 428,                 /* INTEGER  */
    STRING = 429,                  /* STRING  */
    NUMBER = 430,                  /* NUMBER  */
    INV = 431,                     /* INV  */
    LOG = 432,                     /* LOG  */
    EXP = 433,                     /* EXP  */
    SQRT = 434,                    /* SQRT  */
    RSQ = 435,                     /* RSQ  */
    POW = 436,                     /* POW  */
    SIN = 437,                     /* SIN  */
    COS = 438,                     /* COS  */
    SINCOS = 439,                  /* SINCOS  */
    INTDIV = 440,                  /* INTDIV  */
    INTMOD = 441,                  /* INTMOD  */
    INTDIVMOD = 442,               /* INTDIVMOD  */
    SIGNED = 443,                  /* SIGNED  */
    SCALAR = 444,                  /* SCALAR  */
    X = 445,                       /* X  */
    Y = 446,                       /* Y  */
    Z = 447,                       /* Z  */
    W = 448,                       /* W  */
    KERNEL_PRAGMA = 449,           /* KERNEL_PRAGMA  */
    END_KERNEL_PRAGMA = 450,       /* END_KERNEL_PRAGMA  */
    CODE_PRAGMA = 451,             /* CODE_PRAGMA  */
    END_CODE_PRAGMA = 452,         /* END_CODE_PRAGMA  */
    REG_COUNT_PAYLOAD_PRAGMA = 453, /* REG_COUNT_PAYLOAD_PRAGMA  */
    REG_COUNT_TOTAL_PRAGMA = 454,  /* REG_COUNT_TOTAL_PRAGMA  */
    DECLARE_PRAGMA = 455,          /* DECLARE_PRAGMA  */
    BASE = 456,                    /* BASE  */
    ELEMENTSIZE = 457,             /* ELEMENTSIZE  */
    SRCREGION = 458,               /* SRCREGION  */
    DSTREGION = 459,               /* DSTREGION  */
    TYPE = 460,                    /* TYPE  */
    DEFAULT_EXEC_SIZE_PRAGMA = 461, /* DEFAULT_EXEC_SIZE_PRAGMA  */
    DEFAULT_REG_TYPE_PRAGMA = 462, /* DEFAULT_REG_TYPE_PRAGMA  */
    SUBREGNUM = 463,               /* SUBREGNUM  */
    SNDOPR = 464,                  /* SNDOPR  */
    UMINUS = 465,                  /* UMINUS  */
    STR_SYMBOL_REG = 466,          /* STR_SYMBOL_REG  */
    EMPTEXECSIZE = 467             /* EMPTEXECSIZE  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 439 "gram.y"

	char *string;
	int integer;
	double number;
	struct brw_program_instruction instruction;
	struct brw_program program;
	struct region region;
	struct regtype regtype;
	struct brw_reg reg;
	struct condition condition;
	struct predicate predicate;
	struct options options;
	struct declared_register symbol_reg;
	imm32_t imm32;

	struct src_operand src_operand;

#line 294 "gram.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif

/* Location type.  */
#if ! defined YYLTYPE && ! defined YYLTYPE_IS_DECLARED
typedef struct YYLTYPE YYLTYPE;
struct YYLTYPE
{
  int first_line;
  int first_column;
  int last_line;
  int last_column;
};
# define YYLTYPE_IS_DECLARED 1
# define YYLTYPE_IS_TRIVIAL 1
#endif


extern YYSTYPE yylval;
extern YYLTYPE yylloc;

int yyparse (void);


#endif /* !YY_YY_GRAM_H_INCLUDED  */
