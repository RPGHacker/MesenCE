using Avalonia.Data;
using Avalonia.Data.Converters;
using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Config;
using Mesen.Interop;
using Mesen.Utilities;
using Mesen.ViewModels;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Globalization;
using System.Reflection;

namespace Mesen.Debugger.ViewModels
{
	public class RestrictToPlatformValueConverter : IMultiValueConverter
	{
		 public object? Convert(IList<object?> values, Type targetType, object? parameter, CultureInfo culture)
		{
			if (values.Count == 2
				&& values[0] is object propertyOwner
				&& values[1] is CpuType cpuType
				&& parameter is string propertyName)
			{
				if(typeof(bool).IsAssignableTo(targetType) && propertyOwner != null) {
					return IsPropertyAllowedOnPlatform(propertyOwner, propertyName, cpuType);
				}
			}

			return new BindingNotification(new InvalidCastException(), BindingErrorType.Error);
		}

		private bool IsPropertyAllowedOnPlatform(object propertyOwner, string propertyName, CpuType cpuType)
		{
			// Retrieve the method information for MyMethod
			PropertyInfo? propertyInfo = propertyOwner.GetType().GetProperty(propertyName);

			if(propertyInfo == null) {
				return false;
			}

			RestrictToPlatformsAttribute? resrictAttribute = (RestrictToPlatformsAttribute?)Attribute.GetCustomAttribute(propertyInfo, typeof(RestrictToPlatformsAttribute));

			if (resrictAttribute != null)
			{
				foreach(var allowedCpuType in resrictAttribute.SupportedCpuTypes) {
					if (allowedCpuType == cpuType) {
						return true;
					}
				}

				return false;
			}

			return true;
		}		
	}

	public partial class TraceLogOutputConfigurationViewModel : DisposableViewModel
	{
		public TraceLogOutputConfig Config { get; }

		public Enum[] AvailableFormats { get; private set; } = [];
		public Enum[] AvailableCompressionTypes { get; private set; } = [];

		[ObservableProperty] public partial bool IsUsingTextFormat { get; set; } = true;

		[Obsolete("For designer only")]
		public TraceLogOutputConfigurationViewModel() : this(CpuType.Snes, new())
		{
		}

		public TraceLogOutputConfigurationViewModel(CpuType cpuType, TraceLogOutputConfig config)
		{
			Config = config;
			Config.SharedConfig.LastUsedCpuType = cpuType;

			AvailableFormats = FilterEnumByPlatform<TraceLogOutputFormat>(cpuType).ToArray();
			AvailableCompressionTypes = FilterEnumByPlatform<TraceLogOutputCompressionType>(cpuType).ToArray();

			UpdateUsingTextFormat();

			AddDisposable(ReactiveHelper.RegisterRecursiveObserver(Config, OnOptionsChanged));
		}

		// This could be useful in other places as well, so maybe it should move to another location later.
		private static List<Enum> FilterEnumByPlatform<T>(CpuType cpuType) where T : struct, Enum
		{
			return FilterEnumByPlatform<T>(typeof(T), cpuType);
		}

		private static List<Enum> FilterEnumByPlatform<T>(Type enumType, CpuType cpuType) where T : struct, Enum
		{
			T[] values = Enum.GetValuesAsUnderlyingType(enumType) as T[] ?? [];

			List<Enum> result = new();

			foreach(T value in values) {
				var memInfo = enumType.GetMember(value.ToString());
				object[] attributes = memInfo[0].GetCustomAttributes(typeof(RestrictToPlatformsAttribute), false);

				bool exclude = false;

				foreach(var attribute in attributes) {
					if (attribute is RestrictToPlatformsAttribute resrictAttribute) {
						exclude = true;

						foreach(var allowedCpuType in resrictAttribute.SupportedCpuTypes) {
							if (allowedCpuType == cpuType) {
								exclude = false;
								break;
							}
						}
					}
				}

				if (!exclude) {
					result.Add(value);
				}
			}

			return result;
		}


		public void UpdateUsingTextFormat()
		{
			IsUsingTextFormat = !Config.ShowAdvancedSettings || Config.SharedConfig.CpuDependentConfig.Format == TraceLogOutputFormat.Text;
		}

		public void UpdateOptions()
		{
			UpdateUsingTextFormat();
		}

		private void OnOptionsChanged(object? sender, PropertyChangedEventArgs e)
		{
			UpdateOptions();
		}
	}
}
