#pragma once
#include <inttypes.h>
#include "DrakePinD.hpp"

/*
 * Простой обработчик одной кнопки.
 *
 * События (все приходят в один колбек, лишние отключаются маской):
 *   EV_CLICK        - одиночный клик
 *   EV_DOUBLE_CLICK - двойной клик
 *   EV_LONG_HOLD    - долгое нажатие: первый раз через long_ms, затем каждые repeat_ms, пока кнопка удерживается
 *   EV_LONG_RELEASE - отпускание после долгого нажатия
 * Во второй параметр колбека передаётся время нажатия в мс (для кликов - длительность последнего нажатия).
 * Если EV_DOUBLE_CLICK выключен, EV_CLICK приходит сразу после отпускания, без ожидания double_ms.
 * Если оба EV_LONG_* выключены, долгое нажатие считается кликом.
 *
 * Пример:
 *   void OnBtn(DrakeButton::Event ev, uint32_t ms) { ... }
 *
 *   DrakeButton btn({GPIOA, GPIO_PIN_0}, OnBtn);
 *   btn.Init();
 *   btn.SetMask(DrakeButton::EV_CLICK | DrakeButton::EV_LONG_HOLD);
 *   while (1) { btn.Tick(HAL_GetTick()); }
 */

class DrakeButton
{
	enum State
	{
		ST_INIT,			// После Init(), ждём первый Tick()
		ST_DEBOUNCE,		// Начальное состояние кнопки ещё не определено, идёт антидребезг
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

		typedef void (*Callback)(Event ev, uint32_t ms);

		DrakeButton(const DrakePin::PinD_t &pin, Callback callback = nullptr) : _pin(pin, DrakePin::InputPullup, DrakePin::HiZ), _callback(callback),
			_mask(EV_ALL), _debounce_ms(30), _double_ms(300), _long_ms(800), _repeat_ms(1000), _state(ST_INIT), _raw(false),
			_stable(false), _raw_change_time_ms(0), _press_time_ms(0), _timer_ms(0)
		{}

		void Init()
		{
			_pin.Init();
			_stable = false;
			_state = ST_INIT;

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

		void SetTimings(uint16_t debounce_ms, uint16_t double_ms, uint16_t long_ms, uint16_t repeat_ms)
		{
			_debounce_ms = debounce_ms;
			_double_ms = double_ms;
			_long_ms = long_ms;
			_repeat_ms = (repeat_ms != 0) ? repeat_ms : 1;

			return;
		}

		// Начальное состояние кнопки определено
		bool IsReady() const
		{
			return (_state != ST_INIT && _state != ST_DEBOUNCE);
		}

		// Состояние кнопки после антидребезга
		bool IsPressed() const
		{
			return _stable;
		}

		void Tick(uint32_t now_ms)
		{
			bool raw = _ReadPin();

			// Первый Tick() после Init(): запускаем отсчёт антидребезга
			if(_state == ST_INIT)
			{
				_raw = raw;
				_raw_change_time_ms = now_ms;
				_state = ST_DEBOUNCE;

				return;
			}

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

			// Автомат состояний
			switch(_state)
			{
				case ST_INIT:
				{
					break;	// Обработан выше
				}

				case ST_DEBOUNCE:
				{
					if(_stable)
					{
						// Нажатие продержалось debounce_ms
						_press_time_ms = now_ms;
						_state = ST_PRESSED;
					}
					else if((uint32_t)(now_ms - _raw_change_time_ms) >= _debounce_ms)
					{
						// Отпущенное состояние продержалось debounce_ms
						_state = ST_IDLE;
					}

					break;
				}

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
							_timer_ms = now_ms;
							_state = ST_WAIT_SECOND;
						}
						else
						{
							_Emit(EV_CLICK, now_ms - _press_time_ms);
							_state = ST_IDLE;
						}
					}
					else if(_IsLongPress(now_ms))
					{
						_EnterLong(now_ms);
					}

					break;
				}

				case ST_WAIT_SECOND:
				{
					if(press_edge)
					{
						_timer_ms -= _press_time_ms;
						_press_time_ms = now_ms;
						_state = ST_SECOND_PRESSED;
					}
					else if((uint32_t)(now_ms - _timer_ms) >= _double_ms)
					{
						_Emit(EV_CLICK, _timer_ms - _press_time_ms);
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
					else if(_IsLongPress(now_ms))
					{
						// Второе нажатие оказалось долгим: это не двойной клик, а клик + долгое нажатие
						_Emit(EV_CLICK, _timer_ms);
						_EnterLong(now_ms);
					}

					break;
				}

				case ST_LONG:
				{
					if(release_edge)
					{
						_Emit(EV_LONG_RELEASE, now_ms - _press_time_ms);
						_state = ST_IDLE;
					}
					else if((uint32_t)(now_ms - _timer_ms) >= _repeat_ms)
					{
						_timer_ms += _repeat_ms;
						_Emit(EV_LONG_HOLD, now_ms - _press_time_ms);
					}

					break;
				}
			}

			return;
		}

	private:

		bool _ReadPin()
		{
			return (_pin.Read() == DrakePin::Low);
		}

		bool _IsLongPress(uint32_t now_ms) const
		{
			return ((_mask & (EV_LONG_HOLD | EV_LONG_RELEASE)) && (uint32_t)(now_ms - _press_time_ms) >= _long_ms);
		}

		void _EnterLong(uint32_t now_ms)
		{
			_timer_ms = _press_time_ms + _long_ms;
			_state = ST_LONG;
			_Emit(EV_LONG_HOLD, now_ms - _press_time_ms);

			return;
		}

		void _Emit(Event ev, uint32_t ms)
		{
			if(_callback != nullptr && (_mask & ev))
			{
				_callback(ev, ms);
			}

			return;
		}

		DrakePinD _pin;
		Callback _callback;

		uint8_t _mask;
		uint16_t _debounce_ms;
		uint16_t _double_ms;
		uint16_t _long_ms;
		uint16_t _repeat_ms;

		State _state;
		bool _raw;
		bool _stable;
		uint32_t _raw_change_time_ms;	// Когда _raw последний раз изменилось
		uint32_t _press_time_ms;		// Начало текущего нажатия

		// Зависит от состояния:
		//   ST_WAIT_SECOND    - время отпускания после первого нажатия;
		//   ST_SECOND_PRESSED - длительность первого нажатия;
		//   ST_LONG           - время последнего EV_LONG_HOLD.
		uint32_t _timer_ms;
};
