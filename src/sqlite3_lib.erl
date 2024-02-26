%%%-------------------------------------------------------------------
%%% File    : sqlite3_lib.erl
%%% @author Tee Teoh
%%% @copyright 21 Jun 2008 by Tee Teoh
%%% @version 1.0.0
%%% @doc Library module for sqlite3
%%%
%%% @type table_id() = atom() | binary() | string().
%%% @end
%%%-------------------------------------------------------------------
-module(sqlite3_lib).
-include("sqlite3.hrl").

%% API
-export([col_type_to_atom/1]).
-export([value_to_sql/1, value_to_sql_unsafe/1, sql_to_value/1, bin_to_hex/1]).
-export([write_value_sql/1, write_col_sql/1]).
-export([create_table_sql/2, create_table_sql/3, drop_table_sql/1]).
-export([add_columns_sql/2]).
-export([write_sql/2, update_sql/4, update_set_sql/1, delete_sql/3]).
-export([read_sql/1, read_sql/2, read_sql/3, read_sql/4, read_cols_sql/1]).

%%====================================================================
%% API
%%====================================================================
%%--------------------------------------------------------------------
%% @doc Maps sqlite3 column type.
%% @end
%%--------------------------------------------------------------------
-spec col_type_to_string(sql_type()) -> string().
col_type_to_string(integer) ->
    "INTEGER";
col_type_to_string(text) ->
    "TEXT";
col_type_to_string(double) ->
    "REAL";
col_type_to_string(real) ->
    "REAL";
col_type_to_string(blob) ->
    "BLOB";
col_type_to_string(Atom) when is_atom(Atom) ->
    string:to_upper(atom_to_list(Atom));
col_type_to_string(String) when is_list(String) ->
    String.

%%--------------------------------------------------------------------
%% @doc Maps sqlite3 column type.
%% @end
%%--------------------------------------------------------------------
-spec col_type_to_atom(string()) -> atom().
col_type_to_atom("INTEGER") ->
    integer;
col_type_to_atom("TEXT") ->
    text;
col_type_to_atom("REAL") ->
    double;
col_type_to_atom("BLOB") ->
    blob;
col_type_to_atom(String) ->
    list_to_atom(string:to_lower(String)).


%%--------------------------------------------------------------------
%% @doc
%%    Converts an Erlang term to an SQL string.
%%    Currently supports integers, floats, 'null' atom, and iodata
%%    (binaries and iolists) which are treated as SQL strings.
%%
%%    Note that it opens opportunity for injection if an iolist includes
%%    single quotes! Replace all single quotes (') with '' manually, or
%%    use value_to_sql/1 if you are not sure if your strings contain
%%    single quotes (e.g. can be entered by users).
%% @end
%%--------------------------------------------------------------------
-spec value_to_sql_unsafe(sql_value()) -> iolist().
value_to_sql_unsafe(X) ->
    if
        is_binary(X) orelse is_list(X) ->
            [$', binary_or_unicode_to_binary(X), $'];
        true ->
            value_to_sql(X)
    end.

%%--------------------------------------------------------------------
%% @doc
%%    Converts an Erlang term to an SQL string.
%%    Currently supports integers, floats, 'null' atom, and iodata
%%    (binaries and iolists) which are treated as SQL strings.
%%
%%    All single quotes (') will be replaced with ''.
%% @end
%%--------------------------------------------------------------------
-spec value_to_sql(sql_value()) -> iolist().
value_to_sql(X) ->
    case X of
        _ when is_integer(X)   -> integer_to_list(X);
        _ when is_float(X)     -> float_to_list(X);
        true -> "1";
        false -> "0";
        undefined  -> "NULL";
        ?NULL_ATOM -> "NULL";
        {blob, Blob} -> ["x'", bin_to_hex(Blob), $'];
        _ when is_binary(X) orelse is_list(X) ->
            Bin = binary_or_unicode_to_binary(X),
            [$', binary:replace(Bin, <<"'">>, <<"''">>, [global]), $']
    end.

%%--------------------------------------------------------------------
%% @doc
%%    Converts an SQL value to an Erlang term.
%% @end
%%--------------------------------------------------------------------
sql_to_value(String) ->
    case String of
        "NULL" -> null;
        "CURRENT_TIME" -> current_time;
        "CURRENT_DATE" -> current_date;
        "CURRENT_TIMESTAMP" -> current_timestamp;
        [FirstChar | Tail] ->
            case FirstChar of
                $' -> sql_string(Tail);
                $x -> sql_blob(Tail);
                $X -> sql_blob(Tail);
                $+ -> sql_number(Tail);
                $- -> -sql_number(Tail);
                Digit when $0 =< Digit, Digit =< $9 -> sql_number(Tail)
            end
    end.

%%--------------------------------------------------------------------
%% @doc
%%    Creates the values portion of the sql stmt.
%% @end
%%--------------------------------------------------------------------
-spec write_value_sql(sql_value()) -> iolist().
write_value_sql(Values) ->
    map_intersperse(fun value_to_sql/1, Values, ", ").

    
%%--------------------------------------------------------------------
%% @doc Creates the column/data stmt for SQL.
%% @end
%%--------------------------------------------------------------------
-spec write_col_sql([column_id()]) -> iolist().
write_col_sql(Cols) ->
    map_intersperse(fun to_iolist/1, Cols, ", ").

%%--------------------------------------------------------------------
%% @doc Returns copy of IoData with all ' replaced by ''
%% @end
%%--------------------------------------------------------------------
-spec binary_or_unicode_to_binary(binary() | unicode:charlist()) -> binary().
binary_or_unicode_to_binary(Bin) when is_binary(Bin) -> Bin;
binary_or_unicode_to_binary(CharList) when is_list(CharList) -> unicode:characters_to_binary(CharList).

%%--------------------------------------------------------------------
%% @doc Converts a plain binary to its hexadecimal encoding, to be
%%      passed as a blob literal.
%% @end
%%--------------------------------------------------------------------
-spec bin_to_hex(binary()) -> binary().
bin_to_hex(Binary) -> << <<(half_byte_to_hex(X)):8>> || <<X:4>> <= Binary>>.

%%--------------------------------------------------------------------
%% @doc
%%    Creates update set stmt.
%%    Currently supports integer, double/float and strings.
%% @end
%%--------------------------------------------------------------------
-spec update_set_sql([{atom(), sql_value()}]) -> iolist().
update_set_sql(Data) ->
    ColValueToSqlFun =
        fun({Col, Value}) ->
            [atom_to_list(Col), " = ", value_to_sql(Value)]
        end,
    map_intersperse(ColValueToSqlFun, Data, ", ").

%%--------------------------------------------------------------------
%% @doc
%%    Creates list of columns for select stmt.
%% @end
%%--------------------------------------------------------------------
-spec read_cols_sql([column_id()]) -> iolist().
read_cols_sql(Columns) ->
    map_intersperse(fun to_iolist/1, Columns, ", ").

%%--------------------------------------------------------------------
%% @doc Generates a table create stmt in SQL.
%% @end
%%--------------------------------------------------------------------
-spec create_table_sql(table_id(), table_info()) -> iolist().
create_table_sql(Tbl, Columns) ->
    {Type, TName} = encode_table_id(Tbl),
    ["CREATE TABLE ", TName, " (",
     map_intersperse(fun column_sql_for_create_table/1, Columns, ", "),
     ", CHECK ('", Type, "'='", Type, "'));"].

%%--------------------------------------------------------------------
%% @doc Generates a table create stmt in SQL.
%% @end
%%--------------------------------------------------------------------
-spec create_table_sql(table_id(), table_info(), table_constraints()) -> iolist().
create_table_sql(Tbl, Columns, TblConstraints) ->
    {Type, TName} = encode_table_id(Tbl),
    ["CREATE TABLE ", TName, " (",
     map_intersperse(fun column_sql_for_create_table/1, Columns, ", "), ", ",
     table_constraint_sql(TblConstraints),
     ", CHECK ('", Type, "'='", Type, "'));"].

%%--------------------------------------------------------------------
%% @doc Generates a alter table stmt in SQL.
%% @end
%%--------------------------------------------------------------------
-spec add_columns_sql(table_id(),table_info()) -> iolist().
add_columns_sql(Tbl, Columns) ->
    {_, TName} = encode_table_id(Tbl),
    ["ALTER TABLE ", TName, " ADD COLUMN ",
     map_intersperse(fun column_sql_for_create_table/1, Columns, ", "),";"].

%%--------------------------------------------------------------------
%% @doc
%%    Using Key as the column name and Data as list of column names
%%    and values pairs it creates the proper update SQL stmt for the
%%    record with matching Value.
%% @end
%%--------------------------------------------------------------------
-spec update_sql(table_id(), column_id(), sql_value(), [{column_id(), sql_value()}]) -> iolist().
update_sql(Tbl, Key, Value, Data) ->
    {_, TName} = encode_table_id(Tbl),
    ["UPDATE ", TName, " SET ", update_set_sql(Data),
     " WHERE ", to_iolist(Key), " = ", value_to_sql(Value), ";"].

%%--------------------------------------------------------------------
%% @doc Taking Data as list of column names and values pairs it creates the
%%      proper insertion SQL stmt.
%% @end
%%--------------------------------------------------------------------
-spec write_sql(table_id(), [{column_id(), sql_value()}]) -> iolist().
write_sql(Tbl, Data) ->
    {Cols, Values} = lists:unzip(Data),
    ["INSERT INTO ", to_iolist(Tbl), " (", write_col_sql(Cols),
     ") values (", write_value_sql(Values), ");"].

%%--------------------------------------------------------------------
%% @doc Returns all records from table Tbl.
%% @end
%%--------------------------------------------------------------------
-spec read_sql(table_id()) -> iolist().
read_sql(Tbl) ->
    ["SELECT * FROM ", to_iolist(Tbl), ";"].

%%--------------------------------------------------------------------
%% @doc
%%    Returns only specified Columns of all records from table Tbl.
%% @end
%%--------------------------------------------------------------------
-spec read_sql(table_id(), [column_id()]) -> iolist().
read_sql(Tbl, Columns) ->
    ["SELECT ", read_cols_sql(Columns), " FROM ",
     to_iolist(Tbl), ";"].

%%--------------------------------------------------------------------
%% @doc Using Key as the column name searches for the record with
%%      matching Value.
%% @end
%%--------------------------------------------------------------------
-spec read_sql(table_id(), column_id(), sql_value()) -> iolist().
read_sql(Tbl, Key, Value) ->
    ["SELECT * FROM ", to_iolist(Tbl), " WHERE ", to_iolist(Key),
     " = ", value_to_sql(Value), ";"].

%%--------------------------------------------------------------------
%% @doc
%%    Using Key as the column name searches for the record with
%%    matching Value and returns only specified Columns.
%% @end
%%--------------------------------------------------------------------
-spec read_sql(table_id(), column_id(), sql_value(), [column_id()]) -> iolist().
read_sql(Tbl, Key, Value, Columns) ->
    ["SELECT ", read_cols_sql(Columns), " FROM ",
     to_iolist(Tbl), " WHERE ", to_iolist(Key), " = ",
     value_to_sql(Value), ";"].

%%--------------------------------------------------------------------
%% @doc Using Key as the column name searches for the record with
%%      matching Value then deletes that record.
%% @end
%%--------------------------------------------------------------------
-spec delete_sql(table_id(), column_id(), sql_value()) -> iolist().
delete_sql(Tbl, Key, Value) ->
    ["DELETE FROM ", to_iolist(Tbl), " WHERE ", to_iolist(Key),
     " = ", value_to_sql(Value), ";"].

%%--------------------------------------------------------------------
%% @doc Drop the table Tbl from the database
%% @end
%%--------------------------------------------------------------------
-spec drop_table_sql(table_id()) -> iolist().
drop_table_sql(Tbl) ->
    ["DROP TABLE ", to_iolist(Tbl), ";"].

%%====================================================================
%% Internal functions
%%====================================================================

%% @doc Works like string:join for iolists

-spec map_intersperse(fun((X) -> iolist()), [X], iolist()) -> iolist().
map_intersperse(_Fun, [], _Sep) -> [];
map_intersperse(Fun, [Elem], _Sep) -> [Fun(Elem)];
map_intersperse(Fun, [Head | Tail], Sep) -> [Fun(Head), Sep | map_intersperse(Fun, Tail, Sep)].

half_byte_to_hex(X) when X < 10 -> $0 + X;
half_byte_to_hex(X) -> $a + X - 10.

-spec sql_number(string()) -> number() | {error, not_a_number}.
sql_number(NumberStr) ->
    case string:to_integer(NumberStr) of
        {Int, []} ->
            Int;
        _ ->
            case string:to_float(NumberStr) of
                {Float, []} ->
                    Float;
                _ ->
                    {error, not_a_number}
            end
    end.

-spec sql_string(string()) -> binary().
sql_string(StringWithEscapedQuotes) ->
    Res1 = re:replace(StringWithEscapedQuotes, "''", "'",
                      [global, {return, binary}, unicode]),
    erlang:binary_part(Res1, 0, byte_size(Res1) - 1).

-spec sql_blob(string()) -> binary().
sql_blob([$' | Tail]) -> hex_str_to_bin(Tail, <<>>).

hex_str_to_bin("'", Acc) ->
    Acc; %% single quote at the end of blob literal
hex_str_to_bin([X, Y | Tail], Acc) ->
    hex_str_to_bin(Tail, <<Acc/binary, (X * 16 + Y)>>).

column_sql_for_create_table({Name, Type}) ->
    [atom_to_list(Name), " ", col_type_to_string(Type)];
column_sql_for_create_table({Name, Type, Constraints}) ->
    [atom_to_list(Name), " ", col_type_to_string(Type), " ", constraint_sql(Constraints)].

-spec pk_constraint_sql(pk_constraints()) -> iolist().
pk_constraint_sql(Constraint) ->
    case Constraint of
        desc -> "DESC";
        asc -> "ASC";
        autoincrement -> "AUTOINCREMENT";
        _ when is_list(Constraint) -> map_intersperse(fun pk_constraint_sql/1, Constraint, " ")
    end.

-spec constraint_sql(column_constraints()) -> iolist().
constraint_sql(Constraint) ->
    case Constraint of
        primary_key -> "PRIMARY KEY";
        {primary_key, C} -> ["PRIMARY KEY ", pk_constraint_sql(C)];
        unique -> "UNIQUE";
        not_null -> "NOT NULL";
        {default, DefaultValue} -> ["DEFAULT ", value_to_sql(DefaultValue)];
        {raw, S} when is_list(S) -> S;
        _ when is_list(Constraint) -> map_intersperse(fun constraint_sql/1, Constraint, " ")
    end.

-spec table_constraint_sql(table_constraints()) -> iolist().
table_constraint_sql(TableConstraint) ->
    case TableConstraint of
        {primary_key, Columns} ->
            ["PRIMARY KEY(",
             map_intersperse(fun indexed_column_sql/1, Columns, ", "), ")"];
        {unique, Columns} ->
            ["UNIQUE(",
             map_intersperse(fun indexed_column_sql/1, Columns, ", "), ")"];
        {foreign_key, {Columns, Parent, ParentColumns, Action}} ->
            ["FOREIGN KEY(",
             map_intersperse(fun indexed_column_sql/1, Columns, ", "),
             ") REFERENCES ",
             atom_to_list(Parent),
             "(",
             map_intersperse(fun indexed_column_sql/1, ParentColumns, ", "),
             ")",
             Action];
        {raw, S} when is_list(S) -> S;
        _ when is_list(TableConstraint) ->
            map_intersperse(fun table_constraint_sql/1, TableConstraint, ", ")
    end.

indexed_column_sql({ColumnName, asc}) -> [atom_to_list(ColumnName), " ASC"];
indexed_column_sql({ColumnName, desc}) -> [atom_to_list(ColumnName), " DESC"];
indexed_column_sql(ColumnName) -> atom_to_list(ColumnName).


encode_table_id(A) when is_atom(A) ->
    {"am", atom_to_list(A)};
encode_table_id(B) when is_binary(B) ->
    {"bin", B};
encode_table_id(L) when is_list(L) ->
    {"lst", L}.

to_iolist(A) when is_atom(A) ->
    atom_to_list(A);
to_iolist(L) when is_list(L) ->
    L;
to_iolist(B) when is_binary(B) ->
    B.



%%--------------------------------------------------------------------
%% @type sql_value() = number() | 'null' | iodata().
%%
%% Values accepted in SQL statements include numbers, atom 'null',
%% and io:iolist().
%% @end
%%--------------------------------------------------------------------

%%--------------------------------------------------------------------
%% Tests
%%--------------------------------------------------------------------
-ifdef(TEST).
-include_lib("eunit/include/eunit.hrl").

-define(assertFlat(Expected, Value),
        ?assertEqual(iolist_to_binary(Expected), iolist_to_binary(Value))).

quote_test() ->
    ?assertFlat("'abc'", value_to_sql("abc")),
    ?assertFlat("'a''b''''c'", value_to_sql("a'b''c")).

number_test() ->
    ?assertEqual(sql_number(""),    {error, not_a_number}),
    ?assertEqual(sql_number("a"),   {error, not_a_number}),
    ?assertEqual(sql_number(" 1"),  {error, not_a_number}),
    ?assertEqual(sql_number("1 "),  {error, not_a_number}),
    ?assertEqual(sql_number(" 1 "), {error, not_a_number}),
    ?assertEqual(sql_number("0"),   0),
    ?assertEqual(sql_number("0.0"), 0.0),
    ?assertEqual(sql_number("-0.0"), -0.0),
    ?assertEqual(sql_number("+0.0"), 0.0).

create_table_sql_test() ->
    ?assertFlat(
        "CREATE TABLE user (id INTEGER PRIMARY KEY, name TEXT, CHECK ('am'='am'));",
        create_table_sql(user, [{id, integer, [primary_key]}, {name, text}])),
    ?assertFlat(
        "CREATE TABLE user (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, CHECK ('bin'='bin'));",
        create_table_sql(<<"user">>, [{id, integer, [{primary_key, autoincrement}]}, {name, text}])),
    ?assertFlat(
        "CREATE TABLE user (id INTEGER PRIMARY KEY DESC, name TEXT, CHECK ('lst'='lst'));",
        create_table_sql("user", [{id, integer, [{primary_key, desc}]}, {name, text}])),
    ?assertFlat(
        "CREATE TABLE user (id INTEGER, name TEXT, PRIMARY KEY(id), CHECK ('am'='am'));",
        create_table_sql(user,
                         [{id, integer}, {name, text}],
                         [{primary_key, [id]}])).

update_sql_test() ->
    ?assertFlat(
        "UPDATE user SET name = 'a' WHERE id = 1;",
        update_sql(user, id, 1, [{name, "a"}])).

write_sql_test() ->
    ?assertFlat(
        "INSERT INTO user (id, name) values (1, 'a');",
        write_sql(user, [{id, 1}, {name, "a"}])).

read_sql_test() ->
    ?assertFlat(
        "SELECT * FROM user;",
        read_sql(user)),
    ?assertFlat(
        "SELECT id, name FROM user;",
        read_sql(user, [id, name])),
    ?assertFlat(
        "SELECT * FROM user WHERE id = 1;",
        read_sql(user, id, 1)),
    ?assertFlat(
        "SELECT id, name FROM user WHERE id = 1;",
        read_sql(user, <<"id">>, 1, [id, "name"])).

delete_sql_test() ->
    ?assertFlat(
        "DELETE FROM user WHERE id = 1;",
        delete_sql(user, "id", 1)).

drop_table_sql_test() ->
    ?assertFlat(
        "DROP TABLE user;",
        drop_table_sql(user)).

-endif.
