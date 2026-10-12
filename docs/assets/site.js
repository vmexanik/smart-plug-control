/* Навігація, пейджер, кнопки "копіювати", перемикач теми.
 * Щоб додати сторінку — додайте рядок у PAGES і створіть html-файл за зразком. */
(function () {
  'use strict';

  var REPO = 'https://github.com/vmexanik/smart-plug-control';
  var PAGES = [
    { href: 'index.html',        title: 'Огляд' },
    { href: 'hardware.html',     title: 'Підключення' },
    { href: 'setup.html',        title: 'Перше налаштування' },
    { href: 'interface.html',    title: 'Веб-інтерфейс' },
    { href: 'devices.html',      title: 'Розетки та протоколи' },
    { href: 'ac.html',           title: 'Напруга мережі (AC1)' },
    { href: 'maintenance.html',  title: 'Оновлення і скидання' },
    { href: 'faq.html',          title: 'Проблеми та відповіді' }
  ];

  var file = location.pathname.split('/').pop() || 'index.html';
  var cur = -1;
  PAGES.forEach(function (p, i) { if (p.href === file) cur = i; });

  function esc(s) { return String(s).replace(/[&<>"]/g, function (c) { return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]; }); }

  var side = document.getElementById('side');
  if (side) {
    var links = PAGES.map(function (p, i) {
      return '<a href="' + p.href + '"' + (i === cur ? ' aria-current="page"' : '') + '>' + esc(p.title) + '</a>';
    }).join('');
    side.innerHTML =
      '<a class="brand" href="index.html"><img src="assets/favicon.svg" alt="">' +
      '<span>Smart Plug Control<small>інструкція користувача</small></span></a>' +
      '<nav aria-label="Розділи">' + links +
      '<div class="sep"></div>' +
      '<a href="demo/index.html">Інтерактивне демо</a>' +
      '<a class="ext" href="' + REPO + '" target="_blank" rel="noopener">GitHub</a>' +
      '</nav>' +
      '<button class="theme-btn" id="theme-btn" type="button" aria-label="Змінити тему">Тема: авто</button>' +
      '<div class="side-foot">Прошивка v2.1.x · ESP32-S3</div>';
  }

  /* prev / next */
  var main = document.querySelector('main');
  if (main && cur >= 0) {
    var pager = document.createElement('div');
    pager.className = 'pager';
    var html = '';
    if (cur > 0) html += '<a class="prev" href="' + PAGES[cur - 1].href + '"><small>← Назад</small>' + esc(PAGES[cur - 1].title) + '</a>';
    if (cur < PAGES.length - 1) html += '<a class="next" href="' + PAGES[cur + 1].href + '"><small>Далі →</small>' + esc(PAGES[cur + 1].title) + '</a>';
    pager.innerHTML = html;
    main.appendChild(pager);
  }

  /* copy buttons on <pre> */
  Array.prototype.forEach.call(document.querySelectorAll('pre'), function (pre) {
    var b = document.createElement('button');
    b.type = 'button'; b.className = 'copy'; b.textContent = 'Копіювати';
    b.addEventListener('click', function () {
      var text = pre.innerText.replace(/Копіювати$|Скопійовано$/, '').trimEnd();
      var done = function () { b.textContent = 'Скопійовано'; setTimeout(function () { b.textContent = 'Копіювати'; }, 1500); };
      if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(done, done);
      else done();
    });
    pre.appendChild(b);
  });

  /* theme: auto -> light -> dark */
  var btn = document.getElementById('theme-btn');
  var KEY = 'spc-theme';
  var names = { auto: 'авто', light: 'світла', dark: 'темна' };
  function get() { try { return localStorage.getItem(KEY) || 'auto'; } catch (e) { return 'auto'; } }
  function apply(t) {
    var r = document.documentElement;
    if (t === 'auto') r.removeAttribute('data-theme'); else r.setAttribute('data-theme', t);
    if (btn) btn.textContent = 'Тема: ' + names[t];
  }
  apply(get());
  if (btn) btn.addEventListener('click', function () {
    var order = ['auto', 'light', 'dark'];
    var next = order[(order.indexOf(get()) + 1) % 3];
    try { localStorage.setItem(KEY, next); } catch (e) { /* приватний режим */ }
    apply(next);
  });
})();
