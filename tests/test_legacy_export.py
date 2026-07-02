"""Tests for parsing real IBM-i legacy exports.

Covers the graceful-degradation scanner (ADR-0001) and the /COPY + embedded-SQL
lineage extraction (ADR-0002). These exercise the messy shapes that appear in
real S3DL members: labels in the sequence-number area, control-char line
prefixes, free-form calc inside fixed members, and multi-line embedded SQL.
"""

import pytest

from rpg_explainer.analysis import RPGAnalyzer
from rpg_explainer.parser import ParsedFile, RPGParser, sanitize_source


@pytest.fixture
def parser():
    return RPGParser()


@pytest.fixture
def analyzer():
    return RPGAnalyzer()


def _analyze(parser, analyzer, code):
    """Parse a source string and return the analyzed RPGFile."""
    tree = parser.parse_code(code)
    parsed = ParsedFile(path="t.rpgle", tree=tree, source=sanitize_source(code))
    return analyzer.build_index([parsed]).files[0]


class TestNoCascade:
    """A single unmodelled line must not wipe out the rest of the file."""

    def test_copy_with_seq_area_label_then_specs(self, parser, analyzer):
        # `CPY` sits in the sequence-number area (cols 1-5) — a real /COPY
        # export convention. It must not derail the following D-specs.
        code = (
            "CPY  /COPY QSRCC,HSPEC\n"
            "     D myvar           S             10A\n"
            "     D other           S              5P 0\n"
        )
        tree = parser.parse_code(code)
        assert not tree.root_node.has_error
        rpg = _analyze(parser, analyzer, code)
        assert len(rpg.fixed_d_specs) == 2
        assert [c.member for c in rpg.copybooks] == ["HSPEC"]

    def test_free_calc_in_fixed_member_is_opaque(self, parser, analyzer):
        # Free-form calc with a structured-indicator label in a fixed member.
        # We accept it as opaque (unknown_line); the D-spec after it survives.
        code = (
            " B01   If B3_ChkLief(FirmaP:LiLiNr:'I');\n"
            " E01   ENDIF;\n"
            "     D myvar           S             10A\n"
        )
        tree = parser.parse_code(code)
        assert not tree.root_node.has_error
        rpg = _analyze(parser, analyzer, code)
        assert len(rpg.fixed_d_specs) == 1

    def test_fixed_comments_do_not_cascade(self, parser, analyzer):
        code = (
            "     * comment with star in column 6\n"
            "      * comment with star in column 7\n"
            "     D myvar           S             10A\n"
        )
        tree = parser.parse_code(code)
        assert not tree.root_node.has_error
        rpg = _analyze(parser, analyzer, code)
        assert len(rpg.fixed_d_specs) == 1

    def test_control_char_prefixes_are_tolerated(self, parser, analyzer):
        # U+0082 prefixes most lines; U+0016 (SYN) prefixes /COPY lines in the
        # raw export. After sanitising, columns realign and parsing succeeds.
        code = (
            "     * header comment\n"
            "CPY  /COPY QSRCC,HSPEC\n"
            "     D myvar           S             10A\n"
        )
        tree = parser.parse_code(code)
        assert not tree.root_node.has_error
        rpg = _analyze(parser, analyzer, code)
        assert len(rpg.fixed_d_specs) == 1
        assert [c.member for c in rpg.copybooks] == ["HSPEC"]


class TestFreeModeUnaffected:
    """`**FREE` members keep parsing as free-form."""

    def test_free_form_procedure_still_parses(self, parser, analyzer):
        code = (
            "**FREE\n"
            "dcl-proc MyProc;\n"
            "  dcl-pi *n int(10);\n"
            "  end-pi;\n"
            "  return 1;\n"
            "end-proc;\n"
        )
        tree = parser.parse_code(code)
        assert not tree.root_node.has_error
        rpg = _analyze(parser, analyzer, code)
        assert any(p.name == "MyProc" for p in rpg.procedures)


class TestCopybookExtraction:
    def test_multiple_copies_and_includes(self, parser, analyzer):
        code = (
            "CPY  /COPY QSRCC,B3FN_PR\n"
            "CPY  /Include QSRCC,#Prototype\n"
            "      /COPY LIB/SRCF,MEMBER\n"
        )
        rpg = _analyze(parser, analyzer, code)
        members = [c.member for c in rpg.copybooks]
        assert members == ["B3FN_PR", "#Prototype", "MEMBER"]
        src_files = {c.member: c.source_file for c in rpg.copybooks}
        assert src_files["B3FN_PR"] == "QSRCC"
        assert src_files["MEMBER"] == "LIB/SRCF"

    def test_copybook_after_multibyte_char(self, parser, analyzer):
        # Regression: node offsets are byte offsets. A multi-byte umlaut before
        # the directive must not shift/truncate the extracted text.
        code = (
            "     *  geändert: irgendwas\n"  # 'geändert'
            "CPY  /COPY QSRCC,B3FN_PR\n"
        )
        rpg = _analyze(parser, analyzer, code)
        assert [c.member for c in rpg.copybooks] == ["B3FN_PR"]


class TestSqlExtraction:
    def test_insert_is_write(self, parser, analyzer):
        code = "     C     Exec Sql Insert Into B3_SA0085 Values(:WrkRec);\n"
        rpg = _analyze(parser, analyzer, code)
        assert (
            ("INSERT", "B3_SA0085", "write")
            in [(t.operation, t.table, t.direction) for t in rpg.sql_targets]
        )

    def test_select_is_read(self, parser, analyzer):
        code = "        Exec Sql Select COL Into :Var From MYTABLE Where X = 1;\n"
        rpg = _analyze(parser, analyzer, code)
        assert ("SELECT", "MYTABLE", "read") in [
            (t.operation, t.table, t.direction) for t in rpg.sql_targets
        ]

    def test_multiline_statement_is_joined(self, parser, analyzer):
        code = (
            "        Exec Sql\n"
            "          Update ARTIKEL\n"
            "             Set Status = 'X'\n"
            "           Where Id = :Id;\n"
        )
        rpg = _analyze(parser, analyzer, code)
        assert ("UPDATE", "ARTIKEL", "write") in [
            (t.operation, t.table, t.direction) for t in rpg.sql_targets
        ]

    def test_delete_target_not_double_counted_as_read(self, parser, analyzer):
        code = "        Exec Sql Delete From STAGING Where D < :Cut;\n"
        rpg = _analyze(parser, analyzer, code)
        edges = [(t.operation, t.table, t.direction) for t in rpg.sql_targets]
        assert ("DELETE", "STAGING", "write") in edges
        assert ("SELECT", "STAGING", "read") not in edges
