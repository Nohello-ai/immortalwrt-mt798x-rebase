'use strict';
'require view';
'require fs';
'require dom';
'require ui';
'require poll';

/*
 * OpenFi 6C「5G 模组」页面
 *
 * 数据来源:openfi-modem status(读采样进程写的缓存,零副作用)
 *   ★ 页面不直接开 AT 口 —— 串口一次只能一个人用,采样进程正拿着它。
 * AT 终端走 openfi-modem at <命令>,由它自己短暂占用串口。
 *
 * ★ LuCI 的 DOM 坑(照手机截图改的):节点没有 content 属性,
 *   换内容必须用 dom.content(node, 新内容);E() 的 children 里不能放 Promise。
 */

var BIN = '/usr/sbin/openfi-modem';
var POLL_SECS = 10;

function jparse(s) {
	try { return JSON.parse(s || ''); } catch (e) { return null; }
}

var SIM_TXT = { 0: '未知', 1: '就绪', 2: '未插卡', 3: '需要 PIN', 4: '需要 PUK', 5: '故障' };
var REG_TXT = { 0: '未注册', 1: '已注册', 2: '搜索中', 3: '被拒绝', 4: '未知', 5: '漫游' };

function bars(n) {
	n = parseInt(n, 10) || 0;
	return '▮'.repeat(n) + '▯'.repeat(Math.max(0, 5 - n));
}

function tempTxt(v) {
	return (v === null || v === undefined || v <= -100) ? '—' : v + ' °C';
}

function row(k, v) {
	return E('tr', { 'class': 'tr' }, [
		E('td', { 'class': 'td left', 'width': '32%' }, k),
		/* IMEI/ICCID 是 15~20 位连续字符,手机上会把表格撑破 */
		E('td', { 'class': 'td left', 'style': 'word-break:break-all' }, v)
	]);
}

function buildStatus(st) {
	if (!st || st.ok === false) {
		return E('p', { 'class': 'cbi-section-descr' },
			_('读不到模组状态 —— 采样进程可能没在跑,或者没插模组。'));
	}
	return E('table', { 'class': 'table' }, [
		row(_('型号'), st.model || '—'),
		row(_('固件'), st.firmware || '—'),
		row(_('AT 口'), st.at_port || '—'),
		row(_('IMEI'), st.imei || '—'),
		row(_('IMSI'), st.imsi || '—'),
		row(_('ICCID'), st.iccid || '—'),
		row(_('SIM 卡'), _(SIM_TXT[st.sim] || '未知')),
		row(_('运营商'), st.operator || '—'),
		row(_('注网'), REG_TXT[st.reg] !== undefined ? _(REG_TXT[st.reg]) : _('未知')),
		row(_('制式 / 频段'), (st.network || '—') + (st.band ? ' / B' + st.band : '')),
		row(_('信号'), [
			E('strong', {}, bars(st.bars)), ' ',
			'RSRP ' + (st.rsrp <= -1000 ? '—' : st.rsrp + ' dBm'),
			' RSRQ ' + (st.rsrq <= -1000 ? '—' : st.rsrq + ' dB'),
			' SINR ' + (st.sinr <= -1000 ? '—' : st.sinr + ' dB')
		]),
		row(_('模组温度'), 'SOC ' + tempTxt(st.temp_soc) + ' / PA ' + tempTxt(st.temp_pa)),
		row(_('数据口'), st.ifname || '—'),
		row(_('IP'), st.ipaddr || '—'),
		row(_('网关 / DNS'), (st.gateway || '—') + ' / ' + (st.dns1 || '—'))
	]);
}

return view.extend({
	load: function () {
		return fs.exec(BIN, ['status']).then(function (r) {
			return jparse(r.stdout);
		}).catch(function () { return null; });
	},

	render: function (st) {
		var self = this;
		var statusBox = E('div', { 'class': 'cbi-section', 'id': 'modem-status' },
			buildStatus(st));

		function refresh() {
			return fs.exec(BIN, ['status']).then(function (r) {
				st = jparse(r.stdout);
				dom.content(statusBox, buildStatus(st));
			}).catch(function () {});
		}

		var actions = E('div', { 'class': 'cbi-page-actions' }, [
			E('button', {
				'class': 'btn cbi-button cbi-button-apply',
				'click': ui.createHandlerFn(this, function () {
					ui.showModal(_('正在查询模组'), [ E('p', { 'class': 'spinning' }, _('请稍候…')) ]);
					return fs.exec(BIN, ['refresh']).then(function () {
						ui.hideModal();
						return refresh();
					}).catch(function () { ui.hideModal(); });
				})
			}, _('立即查询')),
			' ',
			E('button', {
				'class': 'btn cbi-button cbi-button-reset',
				'click': ui.createHandlerFn(this, function () {
					if (!confirm(_('确定要硬复位模组吗?会断网几十秒。')))
						return;
					return fs.exec(BIN, ['reset']).then(function (r) {
						ui.addNotification(null, E('p', {},
							r.code === 0 ? _('已发出复位') : _('复位失败')),
							r.code === 0 ? 'info' : 'error');
					});
				})
			}, _('硬复位'))
		]);

		/* ---- USB 数据模式 ---- */
		/*
		 * 5G 模组用哪种 USB 数据通道,决定 WAN 怎么出来:
		 *   NCM(5)   wwan0  多包聚合,更快、更干净
		 *   RNDIS(3) usb0   兼容性最好,是模组出厂默认
		 * 开机由 /usr/sbin/openfi-usbmode 自动探测并记住结果,一般不用手动干预。
		 * 这里的按钮只是给它一个入口。
		 */
		var UM_BIN = '/usr/sbin/openfi-usbmode';
		/* 只列可能出现在 state 里的;完整映射在脚本的 mode_name() 里 */
		var UM_MODE = { 0: 'RMNET', 1: 'ECM', 2: 'MBIM', 3: 'RNDIS', 4: 'QMI', 5: 'NCM' };

		/* state 文件是 key=value 一行一条 */
		function umParse(s) {
			var o = {};
			(s || '').split('\n').forEach(function (l) {
				var i = l.indexOf('=');
				if (i > 0)
					o[l.slice(0, i)] = l.slice(i + 1);
			});
			return o;
		}

		function umFmtTime(t) {
			t = parseInt(t, 10);
			if (!t)
				return '-';
			var d = new Date(t * 1000);
			function p2(n) { return (n < 10 ? '0' : '') + n; }
			return d.getFullYear() + '-' + p2(d.getMonth() + 1) + '-' + p2(d.getDate()) + ' '
			     + p2(d.getHours()) + ':' + p2(d.getMinutes());
		}

		var umBody = E('div', {}, E('p', { 'class': 'spinning' }, _('读取中…')));

		function umRefresh() {
			return fs.exec(UM_BIN, [ 'show' ]).then(function (r) {
				var raw = (r.stdout || '').trim();
				if (!raw || raw.indexOf('=') < 0) {
					/* 脚本在没探测过时输出的是中文句子,不是 key=value */
					dom.content(umBody, E('p', {}, _('还没探测过。重启一次,或点下面的「重新探测」。')));
					return;
				}
				var st = umParse(raw);
				var failed = (st.failed || '').trim();

				var rows = [
					[ _('当前模式'), (UM_MODE[st.mode] || _('未知')) + ' (' + (st.mode || '?') + ')' ],
					[ _('网卡'), st.dev || '-' ],
					[ _('协议'), st.proto || '-' ],
					[ _('探测时间'), umFmtTime(st.ts) ],
					[ _('失败记忆'), failed
						? failed.split(/\s+/).map(function (m) {
							return UM_MODE[m] || m;
						  }).join('、') + _('(这些不会再试,7 天后自动清空)')
						: _('无') ]
				];

				var tbl = E('table', { 'class': 'table' }, rows.map(function (kv) {
					return E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td left', 'style': 'width:34%' }, kv[0]),
						E('td', { 'class': 'td left' }, kv[1])
					]);
				}));

				dom.content(umBody, tbl);
			}).catch(function () {
				dom.content(umBody, E('p', {}, _('读取失败(可能没装 openfi-usbmode)。')));
			});
		}

		/*
		 * 探测要 1~2 分钟(每次切模式都要等 USB 重新枚举),
		 * 而 rpcd 的 fs.exec 约 30 秒就超时 —— 上一版就是同步等,
		 * 界面上直接报 "Error: XHR request timed out"。
		 * 所以改成:start 点火(立刻返回)→ 轮询 status 看进度。
		 */
		var umTimer = null;

		function umPoll() {
			return fs.exec(UM_BIN, [ 'status' ]).then(function (r) {
				var raw = (r.stdout || '').trim();
				var parts = raw.split('---');
				var head = umParse(parts[0] || '');
				var st = umParse(parts[1] || '');

				if (head.status === 'running') {
					dom.content(umBody, E('p', { 'class': 'spinning' },
						_('正在探测(约 1~2 分钟,期间网络会断十几秒)…')));
					if (!umTimer)
						umTimer = window.setInterval(function () { umPoll(); }, 3000);
					return;
				}

				if (umTimer) {
					window.clearInterval(umTimer);
					umTimer = null;
				}
				if (head.status === 'done')
					ui.addNotification(null, E('p', {}, _('探测完成')), 'info');
				else if (head.status === 'failed')
					ui.addNotification(null, E('p', {}, _('探测失败,保持原模式')), 'warning');

				/* 结束(或本来就没在跑)→ 回到正常的详情显示 */
				return umRefresh();
			}).catch(function () { return umRefresh(); });
		}

		var umBox = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('USB 数据模式')),
			E('div', { 'class': 'cbi-section-descr' },
				_('5G 模组用哪种 USB 数据通道,决定 WAN 怎么出来。'
				+ 'NCM 更快更干净,RNDIS 兼容性最好(模组出厂默认)。'
				+ '开机自动探测并记住结果,一般不需要手动干预。')),
			umBody,
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-apply',
					'click': ui.createHandlerFn(this, function () {
						if (!confirm(_('重新探测会切换模组的 USB 模式,USB 要重新枚举,'
						             + '网络会断十几秒,最长可能要 1~2 分钟。\n\n继续吗?')))
							return;
						/* ★ 不在这里等下 —— 只点火,进度交给 umPoll 轮询。
						   同步等一定会撞上 rpcd 的 30 秒超时(XHR request timed out)。 */
						return fs.exec(UM_BIN, [ 'start' ]).then(function (r) {
							ui.addNotification(null,
								E('p', {}, r.stdout || _('已开始探测')), 'info');
							return umPoll();
						}).catch(function (e) {
							ui.addNotification(null,
								E('p', {}, _('启动探测失败:') + ' ' + e), 'error');
						});
					})
				}, _('重新探测')),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-reset',
					'click': ui.createHandlerFn(this, function () {
						if (!confirm(_('清除探测记录后,下次开机会重新试一遍(可能要 1~2 分钟)。\n\n继续吗?')))
							return;
						return fs.exec(UM_BIN, [ 'reset' ]).then(function () {
							ui.addNotification(null, E('p', {}, _('已清除,下次开机重新探测')), 'info');
							return umRefresh();
						});
					})
				}, _('清除记录'))
			])
		]);

		/* ---- AT 终端 ---- */
		var atOut = E('pre', {
			'style': 'max-height:16em;overflow:auto;background:rgba(127,127,127,.12);'
			       + 'padding:.6em;white-space:pre-wrap;word-break:break-all'
		}, '');
		var atIn = E('input', { 'type': 'text', 'class': 'cbi-input-text', 'placeholder': 'AT+CSQ' });

		function runAt() {
			var cmd = (atIn.value || '').trim();

			if (!cmd)
				return;
			atOut.textContent += '\n> ' + cmd + '\n';
			return fs.exec(BIN, ['at', cmd]).then(function (r) {
				atOut.textContent += (r.stdout || '') + (r.stderr || '');
				atOut.scrollTop = atOut.scrollHeight;
			}).catch(function (e) {
				atOut.textContent += String(e) + '\n';
			});
		}
		atIn.addEventListener('keydown', function (ev) {
			if (ev.key === 'Enter')
				runAt();
		});

		var atBox = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('AT 终端')),
			E('div', { 'class': 'cbi-section-descr' },
				_('直接向模组发 AT 命令。这里会短暂占用串口,采样进程会稍后重试,不会互相干扰。')),
			/* 手机窄屏时输入框和按钮要能换行 */
			E('div', { 'style': 'display:flex;gap:.5em;flex-wrap:wrap;align-items:center' }, [
				E('div', { 'style': 'flex:1 1 12em;min-width:8em' }, atIn),
				E('button', { 'class': 'btn cbi-button cbi-button-action',
					      'click': ui.createHandlerFn(this, runAt) }, _('发送'))
			]),
			atOut
		]);

		umPoll();
		poll.add(function () { return refresh(); }, POLL_SECS);

		return E('div', {}, [
			E('h2', {}, _('5G 模组')),
			statusBox,
			actions,
			umBox,
			E('h2', {}, _('调试')),
			atBox
		]);
	},

	handleSave: null,
	handleSaveApply: null,
	handleReset: null
});
