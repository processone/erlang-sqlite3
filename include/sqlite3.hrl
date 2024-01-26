-ifdef(DEBUG).
-include_lib("eunit/include/eunit.hrl"). %% for debugging macros
-define(dbg(Message), ?debugMsg(Message)).
-define(dbgF(Format, Data), ?debugFmt(Format, Data)).
-define(dbgVal(Expr), ?debugVal(Expr)).
-define(dbgTime(Text, Expr), ?debugTime(Text, Expr)).
-else.
-define(dbg(_Message), ok).
-define(dbgF(_Format, _Data), ok).
-define(dbgVal(Expr), Expr).
-define(dbgTime(_Text, Expr), Expr).
%% -ifdef(TEST).
%% -include_lib("eunit/include/eunit.hrl"). %% for debugging macros
%% -else.
%% -define(debugMsg(_Message), ok).
%% -define(debugFmt(_Format, _Data), ok).
%% -define(debugVal(Expr), Expr).
%% -define(debugTime(_Text, Expr), Expr).
%% -endif.
-endif.

-define(NULL_ATOM, null).
-type sql_id() :: atom() | binary() | string().
-type table_id() :: sql_id().
-type column_id() :: sql_id().
-type sql_value() :: number() | ?NULL_ATOM | iodata() | {blob, binary()}.
-type sql_type() :: integer | text | double | real | blob | string().

-type pk_constraint() :: autoincrement | desc | asc.
-type pk_constraints() :: pk_constraint() | [pk_constraint()].
-type column_constraint() :: non_null | primary_key | {primary_key, pk_constraints()}
                             | unique | {default, sql_value()}.
-type column_constraints() :: column_constraint() | [column_constraint()].
-type table_constraint() :: {primary_key, [atom()]} | {foreign_key, {[atom()], atom(), [atom()], string()}} | {unique, [atom()]}.
-type table_constraints() :: table_constraint() | [table_constraint()].
-type table_info() :: [{column_id(), sql_type()} | {column_id(), sql_type(), column_constraints()}].

-type sqlite_error() :: {error, integer(), string()} | {error, term()}.
-type sql_params() :: [sql_value() | {atom() | string() | integer(), sql_value()}].
-type sql_non_query_result() :: ok | sqlite_error() | {rowid, integer()}.
-type sql_result() :: sql_non_query_result() | [{columns, [column_id()]} | {rows, [tuple()]} | sqlite_error()].
