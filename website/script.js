const root = document.documentElement;
const themeButton = document.querySelector('.theme-toggle');
themeButton.addEventListener('click', () => {
  root.classList.toggle('dark');
  localStorage.setItem('dzjs-theme', root.classList.contains('dark') ? 'dark' : 'light');
});
if (localStorage.getItem('dzjs-theme') === 'dark') root.classList.add('dark');

const shot = document.querySelector('#interface-shot');
document.querySelectorAll('.interface-tabs button').forEach((button) => {
  button.addEventListener('click', () => {
    document.querySelector('.interface-tabs button.active').classList.remove('active');
    button.classList.add('active');
    shot.src = `assets/${button.dataset.image}`;
    shot.alt = `${button.textContent}界面预览`;
    document.querySelector('.shot-caption b').textContent = button.textContent === '状态总览' ? '清晰的设备状态' : '可配置的防护策略';
  });
});

const modal = document.querySelector('#legal-modal');
const agree = document.querySelector('#agree-check');
const agreeBtn = document.querySelector('#agree-btn');
const gated = document.querySelectorAll('[data-gated]');
let pendingHref = null;

const openModal = (href) => {
  pendingHref = href;
  agree.checked = false;
  agreeBtn.disabled = true;
  modal.hidden = false;
  modal.setAttribute('aria-hidden', 'false');
  document.body.style.overflow = 'hidden';
};
const closeModal = () => {
  pendingHref = null;
  modal.hidden = true;
  modal.setAttribute('aria-hidden', 'true');
  document.body.style.overflow = '';
};

gated.forEach((link) => link.addEventListener('click', (event) => {
  event.preventDefault();
  openModal(link.href);
}));
agree.addEventListener('change', () => { agreeBtn.disabled = !agree.checked; });
agreeBtn.addEventListener('click', () => {
  const href = pendingHref;
  closeModal();
  if (!href) return;
  if (href.includes('download/index')) window.open(href, '_blank', 'noopener');
  else window.location.href = href;
});
modal.querySelectorAll('[data-close]').forEach((el) => el.addEventListener('click', closeModal));
document.addEventListener('keydown', (event) => { if (event.key === 'Escape' && !modal.hidden) closeModal(); });

// —— 版本号与更新公告：从 version.txt 读取 ——
// 格式：每个版本一块，块之间用空行分隔；
//       块内第1行版本号、第2行日期、第3行标题、其余每行一条更新点（减号可省略）。
//       第一块 = 最新版本，驱动全站版本号显示；所有块按顺序渲染进更新日志。
fetch('version.txt')
  .then((res) => (res.ok ? res.text() : Promise.reject(new Error('HTTP ' + res.status))))
  .then((text) => {
    const blocks = text
      .split(/\r?\n\s*\r?\n/)
      .map((block) => block.split(/\r?\n/).map((l) => l.trim()).filter(Boolean))
      .filter((block) => block.length >= 3);
    if (!blocks.length) return;

    const [ver, date] = blocks[0];
    const set = (id, value) => {
      const el = document.getElementById(id);
      if (el && value) el.textContent = value;
    };
    set('hero-date', date);
    set('hero-ver', ver);
    set('stat-ver', ver);
    set('stat-date', `${date} 发布`);
    set('ver-stable', `STABLE / ${ver}`);
    set('ver-direct', `DIRECT / ${ver}`);
    set('ver-date', `完整发布包 · ${date}`);

    const changelog = document.querySelector('.changelog-list');
    const template = changelog && changelog.querySelector('.log-item');
    if (!template) return;
    changelog.textContent = '';
    blocks.forEach(([bVer, bDate, bTitle, ...bItems]) => {
      const item = template.cloneNode(true);
      item.querySelectorAll('[id]').forEach((el) => el.removeAttribute('id'));
      item.querySelector('time').textContent = bDate;
      item.querySelector('.log-ver').textContent = bVer;
      item.querySelector('h3').textContent = bTitle;
      const ul = item.querySelector('ul');
      if (bItems.length) {
        ul.textContent = '';
        bItems.forEach((line) => {
          const li = document.createElement('li');
          li.textContent = line.replace(/^[-•]\s*/, '');
          ul.appendChild(li);
        });
      } else {
        ul.remove();
      }
      changelog.appendChild(item);
    });
  })
  .catch(() => { /* 读取失败（如本地 file:// 打开）时保留页面默认文案 */ });
