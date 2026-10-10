'use strict';
'require view';
'require fs';
'require dom';
'require ui';
'require poll';

/*
 * OpenFi 6C「短信」页面
 *
 * 全部操作走 /usr/bin/openfi-sms 的子命令,页面本身不碰任何文件 ——
 * CLI 和界面走同一条路,权限也只管一个可执行文件。
 * 轮询只调 unread(极轻量),未读数变了才拉完整列表。
 *
 * ★ LuCI 的 DOM 坑(照手机截图改的):节点没有 content 属性,
 *   换内容必须用 dom.content(node, 新内容);E() 的 children 里不能放 Promise。
 */

var BIN = '/usr/bin/openfi-sms';
var POLL_SECS = 10;

function jparse(s) {
	try { return JSON.parse(s || ''); } catch (e) { return null; }
}

function shortAddr(a) {
	a = a || '';
	return a.length > 16 ? a.slice(0, 8) + '…' + a.slice(-5) : a;
}

return view.extend({
	load: function () {
		return fs.exec(BIN, ['list', '100']).then(function (r) {
			return jparse(r.stdout);
		}).catch(function () { return null; });
	},

	render: function (data) {
		var self = this;
		var state = data || { total: 0, unread: 0, messages: [] };
		var lastUnread = state.unread;
		var opened = {};			/* id → 是否展开 */

		var listBox = E('div', { 'class': 'cbi-section', 'id': 'sms-list' });

		function drawList() {
			var msgs = (state && state.messages) || [];

			if (!msgs.length) {
				dom.content(listBox, E('p', { 'class': 'cbi-section-descr' }, _('还没有短信。')));
				return;
			}

			var rows = msgs.map(function (m) {
				var isOpen = !!opened[m.id];

				var head = E('div', {
					'class': 'cbi-section-descr',
					/* 手机上号码/时间 + 两个按钮一行放不下,让它自动换行 */
					'style': 'display:flex;justify-content:space-between;'
					       + 'align-items:center;flex-wrap:wrap;gap:.4em'
				}, [
					E('span', { 'style': 'word-break:break-all' }, [
						E('strong', {}, shortAddr(m.addr) || _('(未知号码)')),
						' ',
						E('small', { 'class': 'text-muted' }, m.ts || '—'),
						' ',
						m.outgoing ? E('span', { 'class': 'label' }, _('已发送')) : '',
						(!m.read && !m.outgoing) ? E('span', { 'class': 'label notice' }, _('未读')) : ''
					]),
					E('span', {}, [
						E('button', {
							'class': 'btn cbi-button cbi-button-action',
							'click': ui.createHandlerFn(self, function () {
								if (opened[m.id]) {
									delete opened[m.id];
									drawList();
									return;
								}
								/* 展开时顺带标已读 */
								return fs.exec(BIN, ['read', m.id]).then(function (r) {
									var one = jparse(r.stdout);

									if (one && one.text) m.text = one.text;
									opened[m.id] = true;
									m.read = true;
									return reload();
								});
							})
						}, isOpen ? _('收起') : _('查看')),
						' ',
						E('button', {
							'class': 'btn cbi-button cbi-button-remove',
							'click': ui.createHandlerFn(self, function () {
								if (!confirm(_('删除这条短信?'))) return;
								return fs.exec(BIN, ['delete', m.id]).then(function () {
									return reload();
								});
							})
						}, _('删除'))
					])
				]);

				var text = E('div', { 'style': 'white-space:pre-wrap;word-break:break-word;margin:.3em 0 .8em 0' },
					isOpen ? (m.text || _('(正文读不出来)')) : '');

				return E('div', {}, [head, text]);
			});

			dom.content(listBox, E('div', {}, [
				E('div', { 'class': 'cbi-section-descr' },
					_('共 %d 条,未读 %d 条(列表最多显示 100 条)').format(state.total, state.unread)),
				E('div', { 'class': 'cbi-page-actions' }, [
					E('button', {
						'class': 'btn cbi-button cbi-button-remove',
						'click': ui.createHandlerFn(self, function () {
							if (!confirm(_('确定要删除全部短信吗?'))) return;
							return fs.exec(BIN, ['delete', 'all']).then(function () { return reload(); });
						})
					}, _('全部删除'))
				])
			].concat(rows)));
		}

		function reload() {
			return fs.exec(BIN, ['list', '100']).then(function (r) {
				state = jparse(r.stdout) || { total: 0, unread: 0, messages: [] };
				lastUnread = state.unread;
				drawList();
			}).catch(function () {});
		}

		drawList();

		/* ---- 发送 ---- */
		var toInput = E('input', { 'type': 'text', 'class': 'cbi-input-text',
					   'placeholder': _('号码,例如 10086') });
		var textArea = E('textarea', { 'class': 'cbi-input-textarea', 'rows': '3',
					       'placeholder': _('短信内容(中文会自动按 UCS2 编码)') });
		var segHint = E('div', { 'class': 'cbi-section-descr' }, '—');

		function updateSegs() {
			var t = textArea.value || '';
			var ucs2 = /[^\x00-\x7F]/.test(t);
			var per1 = ucs2 ? 70 : 160, perN = ucs2 ? 67 : 153;
			var chars = Array.from(t).length;
			var segs = chars <= per1 ? 1 : Math.ceil(chars / perN);

			dom.content(segHint, _('已输入 %d 个字符,将发送 %d 条').format(chars, segs));
		}
		textArea.addEventListener('input', updateSegs);
		updateSegs();

		function doSend() {
			var to = (toInput.value || '').trim();
			var txt = textArea.value || '';

			if (!to)
				return ui.addNotification(null, E('p', {}, _('请填号码')), 'error');
			if (!txt)
				return ui.addNotification(null, E('p', {}, _('请填内容')), 'error');

			return fs.exec(BIN, ['send', to, txt]).then(function (r) {
				var res = jparse(r.stdout);

				if (res && res.ok) {
					ui.addNotification(null, E('p', {},
						_('已发送(共 %d 条)').format(res.segments || 1)), 'info');
					toInput.value = '';
					textArea.value = '';
					updateSegs();
					return reload();
				}
				ui.addNotification(null, E('p', {},
					_('发送失败:') + ' ' + ((res && res.error) || r.stderr || '')), 'error');
			}).catch(function (e) {
				ui.addNotification(null, E('p', {}, _('发送失败:') + ' ' + e), 'error');
			});
		}

		var sendBox = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('发送短信')),
			E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' }, _('号码')), toInput
			]),
			E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' }, _('内容')), textArea
			]),
			segHint,
			E('div', { 'class': 'cbi-page-actions' },
				E('button', {
					'class': 'btn cbi-button cbi-button-apply',
					'click': ui.createHandlerFn(this, doSend)
				}, _('发送')))
		]);

		/* 轮询只查未读数,变了才重新拉列表 */
		poll.add(function () {
			return fs.exec(BIN, ['unread']).then(function (r) {
				var n = parseInt((r.stdout || '0').trim(), 10);

				if (!isNaN(n) && n !== lastUnread)
					return reload();
			}).catch(function () {});
		}, POLL_SECS);

		return E('div', {}, [
			E('h2', {}, _('短信')),
			listBox,
			E('h2', {}, _('撰写')),
			sendBox
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
