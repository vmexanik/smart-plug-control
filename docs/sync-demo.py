#!/usr/bin/env python3
"""Оновлює інтерактивне демо в docs/demo/ з актуального веб-інтерфейсу прошивки.

Запускайте після кожної зміни файлів у main/web/:

    python docs/sync-demo.py

Що робить:
  * копіює main/web/app.js, style.css, favicon.svg у docs/demo/ без змін;
  * з main/web/index.html робить docs/demo/index.html: абсолютні шляхи
    (/app.js) стають відносними (щоб працювало з підкаталогу GitHub Pages)
    і перед app.js підключається docs/demo/mock.js, який підміняє /api/*.

docs/demo/mock.js пишеться вручну й тут не перезаписується. Якщо в прошивці
з'явився новий ендпоінт, додайте його в route() у mock.js.
"""
import re
import shutil
import sys
from pathlib import Path

DOCS = Path(__file__).resolve().parent
WEB = DOCS.parent / "main" / "web"
DEMO = DOCS / "demo"


def fail(msg: str) -> None:
    print(f"ПОМИЛКА: {msg}", file=sys.stderr)
    sys.exit(1)


def main() -> None:
    if not WEB.is_dir():
        fail(f"не знайдено {WEB}")
    if not (DEMO / "mock.js").is_file():
        fail(f"не знайдено {DEMO / 'mock.js'}")

    for name in ("app.js", "style.css", "favicon.svg"):
        shutil.copyfile(WEB / name, DEMO / name)

    html = (WEB / "index.html").read_text(encoding="utf-8")

    # 1) абсолютні шляхи -> відносні
    for old, new in (
        ('href="/favicon.svg"', 'href="favicon.svg"'),
        ('href="/style.css"', 'href="style.css"'),
        ('src="/app.js"', 'src="app.js"'),
    ):
        if old not in html:
            fail(f"в main/web/index.html не знайдено {old}: оновіть sync-demo.py")
        html = html.replace(old, new)

    # 2) mock.js має завантажитись до app.js
    html, n = re.subn(r'(<script src="app\.js"></script>)', r'<script src="mock.js"></script>\n\1', html, count=1)
    if n != 1:
        fail("не вдалося вставити mock.js перед app.js")

    # 3) пошуковики не повинні індексувати копію інтерфейсу
    html = html.replace("<title>", '<meta name="robots" content="noindex">\n<title>', 1)

    (DEMO / "index.html").write_text(html, encoding="utf-8")
    print("Готово: docs/demo/ оновлено з main/web/")


if __name__ == "__main__":
    main()
