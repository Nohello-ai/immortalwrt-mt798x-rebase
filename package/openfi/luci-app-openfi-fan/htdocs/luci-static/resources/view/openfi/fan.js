'use strict';
'require view';
'require form';
'require fs';
'require dom';
'require poll';
'require uci';
'require ui';

/*
 * OpenFi 6C「散热」页面
 *
 * 数据来源:openfi-fan --status(只读,读守护进程每轮写的状态文件)
 *   —— 轮询它不会碰硬件,也不会干扰正在跑的风扇控制。
 * 手动调速走 openfi-fan --speed N,由守护进程去写 PWM,页面不直接碰 /sys。
 *
 * ★ 关于 LuCI 的 DOM 接口(这几个坑是照着手机截图改出来的):
 *   1. 节点【没有】content 属性 —— 换内容要用 dom.content(node, 新内容)。
 *      我原先写的 statusBox.content = ... 是空操作,所以页面上只看到空框。
 *   2. E() 的 children 里【不能放 Promise】—— append 只认节点/数组/函数/字符串,
 *      遇到 Promise 会退化成文本,页面上就会出现 "[object Promise]"。
 *      表单 m.render() 返回的正是 Promise,必须先把结果 then 出来。
 *   3. 'require dom' 要显式写,不然 dom 是 undefined。
 */

var BIN = '/usr/sbin/openfi-fan';
var POLL_SECS = 5;

function jparse(s) {
	try { return JSON.parse(s || ''); } catch (e) { return null; }
}

function fmt(v, unit) {
	return (v === null || v === undefined || v === '') ? '—' : v + (unit || '');
}

function speedText(v) {
	if (v === null || v === undefined) return '—';
	if (v === 0) return _('停转');
	if (v >= 100) return _('全速');
	return v + ' %';
}

var REASON = {
	init: '刚启动', curve: '按温度曲线', silent: '静默档(停转)',
	emergency: '紧急温度(满转)', silent_overheat: '静默档过热兜底',
	manual: '手动指定', hold: '维持(迟滞)'
};

function reasonText(r) {
	return REASON[r] ? _(REASON[r]) : (r || '—');
}

function tempClass(v) {
	if (v === null || v === undefined) return 'text-muted';
	if (v >= 85) return 'text-danger';
	if (v >= 65) return 'text-warning';
	return '';
}

/* 键值表。长值允许断行,不然手机上会把表格撑破。 */
function kvTable(rows) {
	return E('table', { 'class': 'table' }, rows.map(function (r) {
		return E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '34%', 'style': 'word-break:break-all' }, r[0]),
			E('td', { 'class': 'td left', 'style': 'word-break:break-all' }, r[1])
		]);
	}));
}

/* 状态表:纯函数,输入状态对象,输出节点 */
function buildStatus(st) {
	var rows;

	if (!st || st.ok === false) {
		return E('p', { 'class': 'cbi-section-descr' },
			_('读不到状态 —— 守护进程可能没在跑。'));
	}
	rows = [
		[ _('统一温度'), E('strong', { 'class': tempClass(st.temp) }, fmt(st.temp, ' °C')) ],
		[ _('CPU / WiFi / 模组'),
		  fmt(st.cpu, ' °C') + ' / ' + fmt(st.wifi, ' °C') + ' / ' + fmt(st.modem, ' °C') ],
		[ _('当前转速'), E('strong', {}, speedText(st.output)) ],
		[ _('决策原因'), reasonText(st.reason) ],
		[ _('物理开关'), st['switch'] === 'high' ? _('高档')
			: st['switch'] === 'low' ? _('低档') : _('读不到') ],
		[ _('静默模式'), st.silent ? E('strong', { 'class': 'text-warning' }, _('开')) : _('关') ],
		[ _('采样周期'), fmt(st.period, _(' 秒')) ],
		[ _('曲线'), '%s/%s/%s/%s °C → %s/%s/%s/%s %%'.format(
			st.t1, st.t2, st.t3, st.t4, st.s1, st.s2, st.s3, st.s4) ],
		[ _('过热保护'), _('%s °C 以上满转').format(st.emergency_temp) ],
		[ _('静默兜底'), _('%s °C 以上强制开启').format(st.silent_max_temp) ]
	];
	return kvTable(rows);
}

/* 曲线图:原生 SVG,描边用 currentColor 跟随主题配色 */
function buildChart(st) {
	var NS = 'http://www.w3.org/2000/svg';

	if (!st || !st.t1)
		return E('div');

	var W = 520, H = 150, PAD = 34;
	var t0 = 30, t1v = Math.max((st.t4 || 65) + 15, 90);
	var x = function (t) { return PAD + (t - t0) / (t1v - t0) * (W - PAD - 12); };
	var y = function (s) { return H - PAD - s / 100 * (H - PAD - 12); };
	var svg = document.createElementNS(NS, 'svg');

	svg.setAttribute('viewBox', '0 0 ' + W + ' ' + H);
	svg.setAttribute('style', 'width:100%;max-width:520px;height:auto;color:inherit;display:block');

	var pts = [[t0, st.s1], [st.t1, st.s1], [st.t2, st.s2],
		   [st.t3, st.s3], [st.t4, st.s4], [t1v, st.s4]];
	var d = pts.map(function (p, i) {
		return (i ? 'L' : 'M') + x(p[0]).toFixed(1) + ' ' + y(p[1]).toFixed(1);
	}).join(' ');

	[ 'M' + PAD + ' 12 V' + (H - PAD),
	  'M' + PAD + ' ' + (H - PAD) + ' H' + (W - 12) ].forEach(function (dd) {
		var l = document.createElementNS(NS, 'path');

		l.setAttribute('d', dd);
		l.setAttribute('stroke', 'currentColor');
		l.setAttribute('stroke-opacity', '0.35');
		l.setAttribute('fill', 'none');
		svg.appendChild(l);
	});

	var path = document.createElementNS(NS, 'path');

	path.setAttribute('d', d);
	path.setAttribute('stroke', 'currentColor');
	path.setAttribute('stroke-width', '2');
	path.setAttribute('fill', 'none');
	svg.appendChild(path);

	[[st.t1, st.s1], [st.t2, st.s2], [st.t3, st.s3], [st.t4, st.s4]].forEach(function (p) {
		var c = document.createElementNS(NS, 'circle');

		c.setAttribute('cx', x(p[0])); c.setAttribute('cy', y(p[1]));
		c.setAttribute('r', '3'); c.setAttribute('fill', 'currentColor');
		svg.appendChild(c);

		var tx = document.createElementNS(NS, 'text');

		tx.setAttribute('x', x(p[0])); tx.setAttribute('y', y(p[1]) - 7);
		tx.setAttribute('font-size', '10');
		tx.setAttribute('text-anchor', 'middle');
		tx.setAttribute('fill', 'currentColor');
		tx.textContent = p[0] + '°/' + p[1] + '%';
		svg.appendChild(tx);
	});

	if (st.temp !== null && st.temp !== undefined && st.temp >= t0) {
		var cl = document.createElementNS(NS, 'path');

		cl.setAttribute('d', 'M' + x(st.temp) + ' 12 V' + (H - PAD));
		cl.setAttribute('stroke', 'currentColor');
		cl.setAttribute('stroke-dasharray', '3 3');
		cl.setAttribute('stroke-opacity', '0.6');
		cl.setAttribute('fill', 'none');
		svg.appendChild(cl);
	}
	return svg;
}

return view.extend({
	load: function () {
		return Promise.all([
			uci.load('openfi_fan'),
			/* 首次读不到不算错:守护进程可能还没写出状态文件 */
			fs.exec(BIN, ['--status']).catch(function () { return { stdout: '' }; })
		]);
	},

	render: function (data) {
		var self = this;
		var st = jparse(data[1] ? data[1].stdout : '');

		/* 用两个占位容器 + dom.content 局部刷新,而不是整页重建 */
		var statusBox = E('div', { 'class': 'cbi-section', 'id': 'fan-status' },
			buildStatus(st));
		var chartBox = E('div', { 'class': 'cbi-section', 'id': 'fan-chart' },
			buildChart(st));

		function refresh() {
			return fs.exec(BIN, ['--status']).then(function (r) {
				st = jparse(r ? r.stdout : '');
				dom.content(statusBox, buildStatus(st));
				dom.content(chartBox, buildChart(st));
			}).catch(function () {
				dom.content(statusBox, buildStatus(null));
			});
		}

		/* ---- 表单 ---- */
		var m = new form.Map('openfi', _('风扇控制'),
			_('温度曲线是四段阶梯:温度落在哪一段,就用那一段的转速。本板 PWM 是反极性的,界面里的百分比已经是"风扇转速"。'));

		var s1 = m.section(form.NamedSection, 'fan', 'fan', _('基本'));
		s1.anonymous = true;

		var o = s1.option(form.ListValue, 'mode', _('模式'));
		o.value('auto', _('自动(按温度曲线)'));
		o.value('manual', _('手动固定转速'));
		o.rmempty = false;

		o = s1.option(form.Value, 'manual_speed', _('固定转速'),
			_('只在"模式"选手动时生效。留空则回到温度曲线。'));
		o.datatype = 'range(0,100)';
		o.depends('mode', 'manual');

		o = s1.option(form.Value, 'period', _('采样周期'),
			_('每隔几秒采一次温度。越短响应越快,空转唤醒也越多。'));
		o.datatype = 'range(1,60)';
		o.default = '5';

		o = s1.option(form.Value, 'min_speed', _('转速下限'),
			_('温度很低时也不低于这个转速 —— 完全停转在某些机器上会有异响。'));
		o.datatype = 'range(0,100)';
		o.default = '10';

		var s2 = m.section(form.NamedSection, 'fan', 'fan', _('温度曲线'),
			_('四个温度拐点必须递增(相邻至少差 2 °C),对应转速必须单调不减。'));
		s2.anonymous = true;
		[['temp1', _('拐点 1 温度'), '55'], ['speed1', _('拐点 1 转速'), '30'],
		 ['temp2', _('拐点 2 温度'), '58'], ['speed2', _('拐点 2 转速'), '50'],
		 ['temp3', _('拐点 3 温度'), '61'], ['speed3', _('拐点 3 转速'), '80'],
		 ['temp4', _('拐点 4 温度'), '65'], ['speed4', _('拐点 4 转速'), '100']]
		.forEach(function (v) {
			var op = s2.option(form.Value, v[0], v[1]);

			op.datatype = v[0].indexOf('temp') === 0 ? 'range(0,120)' : 'range(0,100)';
			op.default = v[2];
		});

		var s3 = m.section(form.NamedSection, 'fan', 'fan', _('安全'));
		s3.anonymous = true;

		o = s3.option(form.Value, 'emergency_temp', _('紧急温度'),
			_('到这个温度一律满转,不受曲线和静默档影响。'));
		o.datatype = 'range(0,120)';
		o.default = '85';

		o = s3.option(form.Value, 'silent_max_temp', _('静默档过热兜底'),
			_('静默档下风扇本来停转;到这个温度必须强制开,避免过热。'));
		o.datatype = 'range(0,120)';
		o.default = '75';

		o = s3.option(form.ListValue, 'silent_when_hi', _('静默档是哪一档'),
			_('与原厂固件约定一致:默认高电平档对应"关灯/静默"。'));
		o.value('1', _('高档(默认)'));
		o.value('0', _('低档'));
		o.default = '1';

		var s4 = m.section(form.NamedSection, 'fan', 'fan', _('温度来源'));
		s4.anonymous = true;

		o = s4.option(form.Value, 'wifi_iface', _('无线接口名'),
			_('闭源 MTK 驱动的命名:2.4G 是 ra0,5G 是 rax0。'));
		o.default = 'ra0';

		o = s4.option(form.Flag, 'wifi_temp', _('采集无线温度'),
			_('通过 iwpriv 读 WiFi 芯片温度。读不到不影响其他两路。'));
		o.default = '1';

		var manualBtn = E('button', {
			'class': 'btn cbi-button cbi-button-apply',
			'click': ui.createHandlerFn(this, function () {
				/*
				 * 输入数字 = 固定转速;输入 auto 或留空 = 回到温度曲线。
				 * ★ 以前这里只是"写一次 PWM 就完事":守护进程下一个周期
				 *   就把转速覆盖回曲线值,用户看到的效果是"设了但没用"。
				 *   现在它写的是一个守护进程会读的覆盖标记,能一直生效,
				 *   直到用户改回 auto。
				 */
				var v = prompt(_('固定转速(0-100);输入 auto 回到温度曲线'), 'auto');

				if (v === null) return;
				v = (v + '').trim();
				var arg = /^auto$/i.test(v) ? 'auto' : String(parseInt(v, 10));

				if (arg !== 'auto' && (isNaN(parseInt(arg, 10)) ||
				    parseInt(arg, 10) < 0 || parseInt(arg, 10) > 100)) {
					ui.addNotification(null, E('p', {}, _('请输入 0 到 100 的整数,或 auto')), 'error');
					return;
				}
				return fs.exec(BIN, ['--speed', arg]).then(function (r) {
					ui.addNotification(null, E('p', {},
						r.code === 0
							? (arg === 'auto' ? _('已回到温度曲线') : _('已固定为 %d %%').format(parseInt(arg, 10)))
							: _('下发失败')),
						r.code === 0 ? 'info' : 'error');
					return refresh();
				});
			})
		}, _('手动调速一次'));

		/*
		 * ★ m.render() 返回的是 Promise,不能直接塞进 E() 的 children
		 *   (会显示成 "[object Promise]")。所以整个 render 返回一个 Promise:
		 *   LuCI 会 .then() 它。这也是它自己页面的做法。
		 */
		return m.render().then(function (formNodes) {
			poll.add(function () { return refresh(); }, POLL_SECS);

			return E('div', {}, [
				E('h2', {}, _('实时状态')),
				E('div', { 'class': 'cbi-section-descr' },
					_('每 %s 秒自动刷新。这个查询是只读的,不会影响正在运行的风扇控制。').format(POLL_SECS)),
				statusBox,
				chartBox,
				E('div', { 'class': 'cbi-page-actions' }, manualBtn),
				E('h2', {}, _('设置')),
				formNodes
			]);
		});
	},

	/*
	 * 保存并应用。
	 *
	 * ★ 两个坑(都是我自己踩出来的,前端审查指出):
	 *   ① 不能把 handleSave 设成 null —— 这一版 LuCI 的基类 View
	 *      自己定义了 handleSave/handleSaveApply,而 handleSaveApply
	 *      内部会调 this.handleSave。设成 null 之后点"保存并应用"
	 *      会抛 TypeError,而 ui.createHandlerFn 是"先禁用按钮再执行",
	 *      同步异常让 .finally 跑不到 → 按钮永远灰着转圈,只能刷新页面。
	 *   ② 更不能自己写一套 —— 落盘(ui.changes.apply)是基类做的,
	 *      form 的 save 只把改动写进 uci 的暂存区(/tmp/.uci),
	 *      不调 apply 的话配置永远不落盘,提示却显示"已保存"。
	 *
	 * 所以:调基类,成功后再热重载。
	 */
	handleSaveApply: function (ev, mode) {
		return this.super('handleSaveApply', [ev, mode]).then(function () {
			/*
			 * 让守护进程原地重读配置(收到 HUP 重读,风扇不中断)。
			 * ACL 里已授权这条命令;万一失败也要说出来,不能静默吞掉。
			 */
			return fs.exec('/etc/init.d/openfi-fan', ['reload']).catch(function (e) {
				ui.addNotification(null,
					E('p', {}, _('配置已保存,但热重载失败(可能要重启服务):') + ' ' + e),
					'warning');
			});
		});
	}
});
