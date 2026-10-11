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

/*
 * 信号强弱 → 文字 + 颜色
 *
 * 【为什么不用 ▮▯ 那种方块字符】
 *   手机上字体不一定带这两个码位,画出来是空方框或挤成一团,
 *   比不画还难看。改成纯文字 + 颜色,任何设备上都读得懂。
 *
 * 【判断依据】用 RSRP(参考信号接收功率),它是负数,越接近 0 越好:
 *   >= -85   强    绿
 *   >= -100  中    黄
 *   >= -110  弱    橙
 *   更差     很差  红
 */
function sigLevel(rsrp) {
	var r = parseInt(rsrp, 10);

	if (isNaN(r) || r <= -1000)
		return null;
	if (r >= -85)
		return { txt: '强', color: '#2e7d32' };
	if (r >= -100)
		return { txt: '中', color: '#f9a825' };
	if (r >= -110)
		return { txt: '弱', color: '#ef6c00' };
	return { txt: '很差', color: '#c62828' };
}

/* 单个 dB 值的着色(RSRP/RSRQ/SINR 共用) */
function sigColor(v, good, ok) {
	v = parseInt(v, 10);
	if (isNaN(v) || v <= -1000)
		return '';
	return v >= good ? 'color:#2e7d32' : (v >= ok ? 'color:#f9a825' : 'color:#c62828');
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
		row(_('信号'), (function () {
			var lv = sigLevel(st.rsrp);
			var out = [];

			if (lv)
				out.push(E('strong', { 'style': 'color:' + lv.color }, lv.txt), ' ');

			out.push(E('span', { 'style': sigColor(st.rsrp, -85, -100) },
				'RSRP ' + (st.rsrp <= -1000 ? '—' : st.rsrp + ' dBm')));
			out.push(' ');
			out.push(E('span', { 'style': sigColor(st.rsrq, -10, -15) },
				'RSRQ ' + (st.rsrq <= -1000 ? '—' : st.rsrq + ' dB')));
			out.push(' ');
			out.push(E('span', { 'style': sigColor(st.sinr, 13, 0) },
				'SINR ' + (st.sinr <= -1000 ? '—' : st.sinr + ' dB')));
			return out;
		})()),
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
		 *
		 * 【为什么不自动探测】
		 *   探测要逐个模式试,每次都得切模式 → 模组重新枚举 → 网络断十几秒。
		 *   做成自动的,每次开机都要断一次网,还可能反复切。
		 *   所以改成手动:点哪个切哪个,切完自己稍等再刷新。
		 */
		var UM_BIN = '/usr/sbin/openfi-usbmode';
		var UM_MODE = { 1: 'ECM', 3: 'RNDIS', 5: 'NCM' };

		var umBody = E('div', {}, E('p', { 'class': 'spinning' }, _('读取中…')));

		function umParse(txt) {
			var o = {};
			(txt || '').split('\n').forEach(function (l) {
				var i = l.indexOf('=');
				if (i > 0)
					o[l.slice(0, i)] = l.slice(i + 1);
			});
			return o;
		}

		function umRefresh() {
			return fs.exec(UM_BIN, [ 'show' ]).then(function (r) {
				var st = umParse(r.stdout);
				var cur = st.mode || '';

				dom.content(umBody, E('table', { 'class': 'table' }, [
					E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td left', 'style': 'width:34%' }, _('当前模式')),
						E('td', { 'class': 'td left' },
							(UM_MODE[cur] || _('未知')) + (cur ? ' (' + cur + ')' : ''))
					]),
					E('tr', { 'class': 'tr' }, [
						E('td', { 'class': 'td left' }, _('网卡驱动')),
						E('td', { 'class': 'td left' }, st.iface || '-')
					])
				]));
			}).catch(function () {
				dom.content(umBody, E('p', {}, _('读不到(可能没装 openfi-usbmode)。')));
			});
		}

		/* 点一下就发切换命令,不等结果 ——
		   切模式必然要重新枚举,等下去只会撞上 rpcd 的超时。 */
		function umSwitch(mode) {
			return function () {
				return fs.exec(UM_BIN, [ 'set', String(mode) ]).then(function (r) {
					ui.addNotification(null, E('p', {}, r.stdout || _('已发出切换命令')),
						r.code === 0 ? 'info' : 'warning');
					window.setTimeout(function () { umRefresh(); }, 15000);
				}).catch(function (e) {
					ui.addNotification(null, E('p', {}, _('失败:') + ' ' + e), 'error');
				});
			};
		}

		var umBox = E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('USB 数据模式')),
			E('div', { 'class': 'cbi-section-descr' },
				_('5G 模组用哪种 USB 数据通道。模组出厂默认 RNDIS,能上网;'
				+ '切成 NCM 通常更快。'
				+ '切换时模组会重新枚举,网络断十几秒到一分钟,属正常。')),
			umBody,
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'btn cbi-button cbi-button-apply',
					'click': ui.createHandlerFn(this, umSwitch(5))
				}, _('切换到 NCM')),
				' ',
				E('button', {
					'class': 'btn cbi-button cbi-button-reset',
					'click': ui.createHandlerFn(this, umSwitch(3))
				}, _('切换到 RNDIS')),
				' ',
				E('button', {
					'class': 'btn cbi-button',
					'click': ui.createHandlerFn(this, umRefresh)
				}, _('刷新'))
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

		umRefresh();
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
