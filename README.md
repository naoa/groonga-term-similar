# Groonga term similar plugin

Groongaの``TABLE_PAT``型のキーから編集距離をベースとした類似する単語一覧を取得することができます。

ベーシックな[レーベンシュタイン距離](https://en.wikipedia.org/wiki/Levenshtein_distance)(挿入、削除、置換)に加え以下を追加しています。
* [Damerau-Levenshtein](https://en.wikipedia.org/wiki/Damerau%E2%80%93Levenshtein_distance) (文字の並び替えを挿入、削除の２回ではなく１回の並び替えとして扱う)
* qwerty配置のキーボード距離(キーボード上で離れている文字で挿入・置換するほどコストが高い)
* パトリシアトライを使った前方一致検索(先頭が一致するもののみ抽出、演算対象の削減のため)
* (編集距離が近い）キーが含まれるドキュメント数(DF値)が多い順ソート(Indexカラムが必要)

文字の挿入(INSERTION)コスト：10  
文字の削除(DELETION)コスト：10  
文字の置換(SUBSTITUTION)コスト：10  
文字の並び替え(TRANSPOSITION)コスト：10  
キーボード距離コスト：1~12... 　以下の2次元座標上のユークリッド距離（直線距離）

```
     1   2   3   4   5   6   7   8   9  10  11  12  13
  ------------------------------------------------------
 1|  `   1   2   3   4   5   6   7   8   9   0   -   =
 2|  q   w   e   r   t   y   u   i   o   p   [   ]   \
 3|  a   s   d   f   g   h   j   k   l   ;   '
 4|  z   x   c   v   b   n   m   ,   .   /
 5|                 ' '

     1   2   3   4   5   6   7   8   9  10  11  12  13
  ------------------------------------------------------
 1|  ~   !   @   #   $   %   ^   &   *   (   )   +
 2|  Q   W   E   R   T   Y   U   I   O   P   {   }   |
 3|  A   S   D   F   G   H   J   K   L   :   "
 4|  Z   X   C   V   B   N   M   <   >   ?
 5|                 ' '
```


### ```table_term_similar```コマンド
あらかじめ単語が蓄積された``TABLE_PAT``型のテーブルと所定のキーを入力することにより、編集距離、キーボード距離が短い類似語が出力されます。
以下の例では``databasw``という単語で``database``が最も上位に出力されています。これは、キーボード上でwから距離が1であるeに1回置換されたことを示しています(10+1)。

```
plugin_register commands/term_similar
[[0,0.0,0.0],true]
table_create Tags TABLE_PAT_KEY ShortText UInt32
[[0,0.0,0.0],true]
load --table Tags
[
{"_key": "Databaso"},
{"_key": "Database"},
{"_key": "DataSystem"},
{"_key": "Groonga"}
]
[[0,0.0,0.0],4]
table_term_similar "Databasw" Tags
[
  [
    0,
    0.0,
    0.0
  ],
  [
    [
      2
    ],
    [
      [
        "_key",
        "ShortText"
      ],
      [
        "_score",
        "Int32"
      ]
    ],
    [
      "Database",
      11
    ],
    [
      "Databaso",
      17
    ]
  ]
]
```

#### オプション
| arg        | description |default|
|:-----------|:------------|:------|
| term      | 単語 | NULL |
| table     | TABLE_PAT_KEY型のテーブル | NULL |
| prefix_length | 前方一致検索の文字数. 小数点以下を指定すると比率指定 | 3 |
| distance_threshold | 出力する距離(コスト)の閾値 | 30 |
| df_threshold | IndexのDF値の閾値(指定すると最終スコアはDF値) | 0 |
| limit | 出力数を指定 | -1 |


### ```term_similar```関数
上記コマンドの関数バージョン。output_columnsに指定することにより、テーブルすべての類似語を一気に取得可能。

```
plugin_register commands/term_similar
[[0,0.0,0.0],true]
table_create Tags TABLE_PAT_KEY ShortText UInt32
[[0,0.0,0.0],true]
table_create Memos TABLE_HASH_KEY ShortText
[[0,0.0,0.0],true]
column_create Memos tags COLUMN_VECTOR Tags
[[0,0.0,0.0],true]
column_create Tags index COLUMN_INDEX Memos tags
[[0,0.0,0.0],true]
load --table Memos
[
{"_key": "Nroonga", "tags": ["NodeJS", "Nroonga", "Databaso"]},
{"_key": "MySQL", "tags": ["MySQL", "Databaso"]},
{"_key": "PGroonga", "tags": ["PostgreSQL", "Groonga", "Database"]},
{"_key": "Groonga", "tags": ["Groonga", "Database"]},
{"_key": "Rroonga", "tags": ["Rroonga", "Database"]},
{"_key": "Mroonga", "tags": ["MySQL", "Mroonga", "Database"]},
{"_key": "Droonga", "tags": ["Dag", "Database"]},
{"_key": "PostgreSQL", "tags": ["PostgreSQL", "Databas"]}
]
[[0,0.0,0.0],8]
select Tags --filter 'term_similar(_key,"Tags")' --output_columns '_key,term_similar(_key,"Tags")' --command_version 2
[
  [
    0,
    0.0,
    0.0
  ],
  [
    [
      [
        2
      ],
      [
        [
          "_key",
          "ShortText"
        ],
        [
          "term_similar",
          "Object"
        ]
      ],
      [
        "Databas",
        "Database"
      ],
      [
        "Databaso",
        "Database"
      ]
    ]
  ]
]
```

#### オプション
| arg        | description |default|
|:-----------|:------------|:------|
| term      | 単語 or output_column | NULL |
| table_name     | TABLE_PAT_KEY型のテーブル名 | NULL |
| prefix_length | 前方一致検索の文字数. 小数点以下を指定すると比率指定 | 3 |
| distance_threshold | 出力する距離(コスト)の閾値 | 25 |
| df_threshold | IndexのDF値の閾値 | 5 |
| min_length | 演算対象とする最短文字数 | 5 |


### ```TokenFilterTypo``` トークンフィルター
検索時に語彙表にトークンに対応するキーが見つからなかった場合、編集距離、キーボード距離が近いトークンに変更します。

Typo等で検索結果が０になるのを防げます。距離が近いトークンすべてがヒットするわけではありません（あいまい検索ではない）。

```
plugin_register commands/term_similar
[[0,0.0,0.0],true]
table_create Memos TABLE_NO_KEY
[[0,0.0,0.0],true]
column_create Memos content COLUMN_SCALAR ShortText
[[0,0.0,0.0],true]
table_create Terms TABLE_PAT_KEY ShortText   --default_tokenizer TokenBigram   --normalizer NormalizerAuto   --token_filters TokenFilterTypo
[[0,0.0,0.0],true]
column_create Terms memos_content COLUMN_INDEX|WITH_POSITION Memos content
[[0,0.0,0.0],true]
load --table Memos
[
{"content": "Groonga is fastest database"}
]
[[0,0.0,0.0],1]
select Memos --match_columns content --query "databasw"
[
  [
    0,
    0.0,
    0.0
  ],
  [
    [
      [
        1
      ],
      [
        [
          "_id",
          "UInt32"
        ],
        [
          "content",
          "ShortText"
        ]
      ],
      [
        1,
        "Groonga is fastest database"
      ]
    ]
  ]
]
```

#### オプション
以下を環境変数で指定可能

| env        | description |default|
|:-----------|:------------|:------|
| GRN_TERM_SIMILAR_PREFIX_RATIO | 前方一致検索の文字数を比率指定 | 0.8 |
| GRN_TERM_SIMILAR_DISTANCE_THRESHOLD | 出力する距離(コスト)の閾値 | 13 |
| GRN_TERM_SIMILAR_DF_THRESHOLD | IndexのDF値の閾値 | 0 |
| GRN_TERM_SIMILAR_MIN_LENGTH | 演算対象とする最短文字数 | 4 |


### ```keyboard_distance```関数

組み込みの``edit_distance``関数に上記の並び替え、キーボード距離を追加したもの。
上記の``table_term_similar``コマンド, ``term_similar``関数内で利用されている。

```
plugin_register commands/term_similar
[[0,0.0,0.0],true]
table_create Tags TABLE_PAT_KEY ShortText UInt32
[[0,0.0,0.0],true]
load --table Tags
[
{"_key": "Groonga"}
]
[[0,0.0,0.0],1]
select Tags --output_columns 'keyboard_distance("Base", "Basd")' --limit 1 --command_version 2
[[0,0.0,0.0],[[[1],[["keyboard_distance","Object"]],[11.0]]]]
select Tags --output_columns 'keyboard_distance("Base", "Basp")' --limit 1 --command_version 2
[[0,0.0,0.0],[[[1],[["keyboard_distance","Object"]],[17.0]]]]
```

#### オプション
| arg        | description |default|
|:-----------|:------------|:------|
| 1      | 単語1 | NULL |
| 2     | 単語2| NULL |
| 3     | transpositionを含めるか | true |
| 4     | keyboardを含めるか | true |

### ```edit_distance_bp```関数

ビットパラレル法により高速に編集距離を算出する。
64ビットまでの１バイト文字列のみに対応。日本語は未対応。

#### オプション
| arg        | description |default|
|:-----------|:------------|:------|
| 1      | 単語1 | NULL |
| 2     | 単語2| NULL |
| 3     | transpositionを含めるか | true |


## Install

Install libgroonga-dev / groonga-devel

Build this command.

    % sh autogen.sh
    % ./configure
    % make
    % sudo make install

## Usage

Register `commands/term_similar`:

    % groonga DB
    > plugin_register commands/term_similar

Now, you can use `table_term_similar` command and `term_similar` function

## Author

Naoya Murakami naoya@createfield.com

## License

LGPL 2.1. See COPYING for details.
