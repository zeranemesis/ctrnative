"""The 3DS catalogue (docs/Catalogue Nintendo 3DS.xlsx), a game's line marked after each test:
    python tools/catalogue.py "<the ROM's file name>" --works Oui|Partiel|Non [--fps 60] [--scope "..."] [--notes "..."]
The line is the cartridge edition of that name (its No-Intro name, the ROM's file name without .3ds). The tracking
columns are added to the table the first time, with a count at the top."""
import argparse
import copy
import datetime
from pathlib import Path

from openpyxl import load_workbook
from openpyxl.worksheet.table import TableColumn

HERE = Path(__file__).resolve().parent
DEFAULT = HERE.parent / "docs" / "Catalogue Nintendo 3DS.xlsx"
COLUMNS = [("Testé (ctrnative)", 14), ("Fonctionnel", 14), ("Images/s", 11), ("Ce qui est testé", 60),
           ("Date du test", 14), ("Remarques", 70)]
NAME_COLUMN = "Nom de l’édition"


def tracking(ws):
    """the tracking columns' first index (1-based), added to the table when missing"""
    table = ws.tables["Cartouches3DS"]
    first_col, header_row = ws[table.ref.split(":")[0]].column, ws[table.ref.split(":")[0]].row
    last = ws[table.ref.split(":")[1]]
    headers = [ws.cell(header_row, c).value for c in range(first_col, last.column + 1)]
    if COLUMNS[0][0] in headers:
        return first_col + headers.index(COLUMNS[0][0]), header_row, last.row
    start = last.column + 1
    model_header, model_cell = ws.cell(header_row, last.column), ws.cell(header_row + 1, last.column)
    for k, (name, width) in enumerate(COLUMNS):
        col = start + k
        h = ws.cell(header_row, col, name)
        h._style = copy.copy(model_header._style)
        for row in range(header_row + 1, last.row + 1):
            ws.cell(row, col)._style = copy.copy(model_cell._style)
        ws.column_dimensions[h.column_letter].width = width
        table.tableColumns.append(TableColumn(id=len(table.tableColumns) + 1, name=name))
    end = ws.cell(last.row, start + len(COLUMNS) - 1).coordinate
    table.ref = f"{table.ref.split(':')[0]}:{end}"
    if table.autoFilter is not None:
        table.autoFilter.ref = table.ref
    # the count, above the columns, a formula over them
    tested, works = (ws.cell(header_row + 1, start + k).column_letter for k in (0, 1))
    rows = f"{header_row + 1}:{last.row}"
    a, b = rows.split(":")
    summary = ws.cell(header_row - 1, start)
    summary.value = (f'="Testés : "&COUNTIF({tested}{a}:{tested}{b},"Oui")&"  ·  fonctionnels : "&'
                     f'COUNTIF({works}{a}:{works}{b},"Oui")&"  ·  en partie : "&COUNTIF({works}{a}:{works}{b},"Partiel")&'
                     f'"  ·  non : "&COUNTIF({works}{a}:{works}{b},"Non")')
    summary._style = copy.copy(ws.cell(header_row - 1, 1)._style)
    return start, header_row, last.row


def mark(path, rom, works, fps=None, scope=None, notes=None, date=None):
    wb = load_workbook(path)
    ws = wb["Cartouches"]
    start, header_row, last_row = tracking(ws)
    headers = [c.value for c in ws[header_row]]
    name_col = headers.index(NAME_COLUMN) + 1
    edition = rom[:-4] if rom.lower().endswith(".3ds") else rom
    rows = [r for r in range(header_row + 1, last_row + 1) if ws.cell(r, name_col).value == edition]
    if not rows:
        raise SystemExit(f"no cartridge edition named {edition!r} in the catalogue")
    r = rows[0]
    ws.cell(r, start, "Oui")
    ws.cell(r, start + 1, works)
    if fps is not None:
        ws.cell(r, start + 2, fps)
    if scope is not None:
        ws.cell(r, start + 3, scope)
    ws.cell(r, start + 4, date or datetime.date.today())
    ws.cell(r, start + 4).number_format = "DD/MM/YYYY"
    if notes is not None:
        ws.cell(r, start + 5, notes)
    wb.save(path)
    print(f"{edition}: line {r}, {works}")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("rom")
    p.add_argument("--works", required=True, choices=["Oui", "Partiel", "Non"])
    p.add_argument("--fps")
    p.add_argument("--scope")
    p.add_argument("--notes")
    p.add_argument("--file", default=str(DEFAULT))
    a = p.parse_args()
    mark(a.file, a.rom, a.works, a.fps, a.scope, a.notes)
