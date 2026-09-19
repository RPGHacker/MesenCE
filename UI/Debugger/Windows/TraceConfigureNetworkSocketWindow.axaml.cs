using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Layout;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using Mesen.Config;
using Mesen.Config.Shortcuts;
using Mesen.Interop;
using Mesen.Localization;
using Mesen.Utilities;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Linq;

namespace Mesen.Windows
{
	public enum TraceConfigureNetworkSocketWindowResult
	{
		Cancelled,
		ConnectionEstablished,
	}

	public class TraceConfigureNetworkSocketWindow : MesenWindow
	{
		public ushort Port { get; set; } = 27015;
		public TraceFormat TraceFormat { get; set; } = TraceFormat.Text;
		public bool UniqueRowsOnly { get; set; } = true;

		public TraceConfigureNetworkSocketWindow()
		{
			InitializeComponent();
		}

		protected override void OnClosed(EventArgs e)
		{
			base.OnClosed(e);
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}

		protected override void OnOpened(EventArgs e)
		{
			base.OnOpened(e);
		}

		protected override void OnClosing(WindowClosingEventArgs e)
		{
			base.OnClosing(e);
		}

		private async void OnStartLogging(object sender, RoutedEventArgs e)
		{
			if(DebugApi.OpenNetworkLogSocket(Port)) {
				TraceNetworkSocketWaitForConnectionWindow wnd = new TraceNetworkSocketWaitForConnectionWindow();
				TraceNetworkSocketWaitForConnectionWindowResult result = await wnd.ShowCenteredDialog<TraceNetworkSocketWaitForConnectionWindowResult>(this.GetWindow());

				if (result == TraceNetworkSocketWaitForConnectionWindowResult.Cancelled) {
					DebugApi.CloseNetworkLogSocket();
					return;
				}

				Debug.Assert(result == TraceNetworkSocketWaitForConnectionWindowResult.ConnectionEstablished);

				this.Close(TraceConfigureNetworkSocketWindowResult.ConnectionEstablished);
			} else {
				await MesenMsgBox.Show(null, "CreatingTraceLogNetworkSocketFailed", MessageBoxButtons.OK, MessageBoxIcon.Error);
			}
		}

		private void OnCancel(object sender, RoutedEventArgs e)
		{
			this.Close(TraceConfigureNetworkSocketWindowResult.Cancelled);
		}
	}
}
