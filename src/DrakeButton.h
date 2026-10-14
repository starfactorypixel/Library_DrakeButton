#pragma once
#include <inttypes.h>
#include "DrakePinD.hpp"

/*
 * Простой обработчик одной кнопки.
 *
 * События (все приходят в один колбек):
 *   EV_CLICK        - одиночный клик
 *   EV_DOUBLE_CLICK - двойной клик
 *   EV_LONG_HOLD    - долгое нажатие; первый раз через long_ms, затем каждые repeat_ms, пока кнопка удерживается
 *   EV_LONG_RELEASE - отпускание после долгого нажатия
 * Во второй параметр колбека передаётся суммарное время нажатия в мс (для кликов - длительность последнего нажатия).
 *
 * Пример:
 *   void OnBtn(DrakeButton::Event ev, uint32_t ms) { ... }
 *
 *   DrakeButton btn({GPIOA, GPIO_PIN_0}, HAL_GetTick, OnBtn);
 *   btn.SetMask(DrakeButton::EV_CLICK | DrakeButton::EV_LONG_HOLD);
 *   while (1) { btn.Tick(); }
 */

class DrakeButton
{
	enum State
	{
		ST_IDLE,
		ST_PRESSED,
		ST_WAIT_SECOND,
		ST_SECOND_PRESSED,
		ST_LONG
	};
	
	public:
		
		enum Event
		{
			EV_CLICK        = 0x01,
			EV_DOUBLE_CLICK = 0x02,
			EV_LONG_HOLD    = 0x04,
			EV_LONG_RELEASE = 0x08,
			EV_ALL          = 0x0F
		};
		
		typedef uint32_t (*TimeFn)();
		typedef bool (*ReadFn)();
		typedef void (*Callback)(Event ev, uint32_t ms);
		
		DrakeButton(const DrakePin::PinD_t &pin, TimeFn time_fn, Callback callback = nullptr) : _pin(pin, DrakePin::Input, DrakePin::HiZ), _time_fn(time_fn), _callback(callback), 
			_mask(EV_ALL), _debounce_ms(30), _double_ms(300), _long_ms(800), _repeat_ms(1000), _state(ST_IDLE), _raw(false), 
			_stable(false), _raw_change_time_ms(0), _press_time_ms(0), _release_time_ms(0), _next_long_ms(0)
		{}
		
		void Init()
		{
			_pin.Init();

			return;
		}
		
		void SetCallback(Callback callback)
		{
			_callback = callback;
			
			return;
		}
		
		void SetMask(uint8_t mask)
		{
			_mask = mask;
			
			return;
		}
		
		uint8_t GetMask() const
		{
			return _mask;
		}
		
		void SetTimings(uint16_t debounce_ms, uint16_t double_ms, uint32_t long_ms, uint32_t repeat_ms)
		{
			_debounce_ms = debounce_ms;
			_double_ms = double_ms;
			_long_ms = long_ms;
			_repeat_ms = (repeat_ms != 0) ? repeat_ms : 1;
			
			return;
		}
		
		bool IsPressed()
		{
			return (_pin.Read() == DrakePin::High);
		}
		
		bool IsPressedStable() const
		{
			return _stable;
		}
		
		void Tick()
		{
			uint32_t now_ms = _time_fn();
			
			// --- антидребезг ---
			bool raw = IsPressed();
			if(raw != _raw)
			{
				_raw = raw;
				_raw_change_time_ms = now_ms;
			}
			
			bool press_edge = false;
			bool release_edge = false;
			if(_raw != _stable && (uint32_t)(now_ms - _raw_change_time_ms) >= _debounce_ms)
			{
				_stable = _raw;
				if(_stable)
				{
					press_edge = true;
				}
				else
				{
					release_edge = true;
				}
			}
			
			// --- автомат состояний ---
			switch (_state)
			{
				case ST_IDLE:
				{
					if(press_edge)
					{
						_press_time_ms = now_ms;
						_state = ST_PRESSED;
					}
					
					break;
				}

				case ST_PRESSED:
				{
					if(release_edge)
					{
						if(_mask & EV_DOUBLE_CLICK)
						{
							_release_time_ms = now_ms;
							_state = ST_WAIT_SECOND;
						}
						else
						{
							_Emit(EV_CLICK, now_ms - _press_time_ms);
							_state = ST_IDLE;
						}
					}
					else if((_mask & (EV_LONG_HOLD | EV_LONG_RELEASE)) && (uint32_t)(now_ms - _press_time_ms) >= _long_ms)
					{
						_state = ST_LONG;
						_next_long_ms = _long_ms + _repeat_ms;
						_Emit(EV_LONG_HOLD, now_ms - _press_time_ms);
					}
					
					break;
				}

				case ST_WAIT_SECOND:
				{
					if(press_edge)
					{
						_press_time_ms = now_ms;
						_state = ST_SECOND_PRESSED;
					}
					else if((uint32_t)(now_ms - _release_time_ms) >= _double_ms)
					{
						_Emit(EV_CLICK, _release_time_ms - _press_time_ms);
						_state = ST_IDLE;
					}
					
					break;
				}

				case ST_SECOND_PRESSED:
				{
					if(release_edge)
					{
						_Emit(EV_DOUBLE_CLICK, now_ms - _press_time_ms);
						_state = ST_IDLE;
					}
					
					break;
				}

				case ST_LONG:
				{
					uint32_t held_ms = now_ms - _press_time_ms;
					if(release_edge)
					{
						_Emit(EV_LONG_RELEASE, held_ms);
						_state = ST_IDLE;
					}
					else if(held_ms >= _next_long_ms)
					{
						_Emit(EV_LONG_HOLD, held_ms);
						_next_long_ms += _repeat_ms;
					}

					break;
				}
			}
			
			return;
		}
		
	private:
		
		void _Emit(Event ev, uint32_t ms)
		{
			if(_callback != nullptr && (_mask & ev))
			{
				_callback(ev, ms);
			}
			
			return;
		}
		
		DrakePinD _pin;
		TimeFn _time_fn;
		Callback _callback;

		uint8_t _mask;
		uint16_t _debounce_ms;
		uint16_t _double_ms;
		uint32_t _long_ms;
		uint32_t _repeat_ms;

		uint8_t _state;
		bool _raw;
		bool _stable;
		uint32_t _raw_change_time_ms;
		uint32_t _press_time_ms;
		uint32_t _release_time_ms;
		uint32_t _next_long_ms;
};
